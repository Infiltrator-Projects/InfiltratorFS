#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Publish one higher checkpoint whose allocation tree frees a live index child.

The two older checkpoint replicas and their allocation tree are left untouched.
The synthetic higher generation uses a complete replacement allocation tree, so
checkpoint selection must reject it because the index child is marked free,
fall back to the previous generation, and heal the divergent checkpoint.
"""

import os
import struct
import sys


BLOCK_SIZE = 4096
CHECKSUM_OFFSET = 220
CHECKSUM_SIZE = 32
ALLOCATION_HEADER_SIZE = 72
ALLOCATION_CHECKSUM_OFFSET = 40
ALLOCATION_CHECKSUM_SIZE = 32
ALLOCATION_DATA_SIZE = BLOCK_SIZE - ALLOCATION_HEADER_SIZE
ALLOCATION_BITS_PER_LEAF = ALLOCATION_DATA_SIZE * 8
ALLOCATION_FANOUT = ALLOCATION_DATA_SIZE // 8
ALLOCATION_ROOT_LEVEL = 3
CHECKSUM_CRC64_ECMA = 1
POLYNOMIAL = 0x42F0E1EBA9EA3693


def crc64(block: bytes) -> int:
    crc = 0
    for byte in block:
        crc ^= byte << 56
        for _ in range(8):
            if crc & (1 << 63):
                crc = ((crc << 1) ^ POLYNOMIAL) & 0xFFFFFFFFFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFFFFFFFFFF
    return crc


def ceil_div(value: int, divisor: int) -> int:
    return (value + divisor - 1) // divisor


def bit_get(bitmap: bytearray, block: int) -> bool:
    return bool(bitmap[block >> 3] & (1 << (block & 7)))


def bit_set(bitmap: bytearray, block: int, used: bool) -> None:
    mask = 1 << (block & 7)
    if used:
        bitmap[block >> 3] |= mask
    else:
        bitmap[block >> 3] &= ~mask & 0xFF


def read_block(image, block: int) -> bytearray:
    image.seek(block * BLOCK_SIZE)
    data = bytearray(image.read(BLOCK_SIZE))
    if len(data) != BLOCK_SIZE:
        raise ValueError(f"short read at block {block}")
    return data


def write_block(image, block: int, data: bytes) -> None:
    if len(data) != BLOCK_SIZE:
        raise ValueError("allocation page must be exactly one block")
    image.seek(block * BLOCK_SIZE)
    image.write(data)


def validate_crc(block: bytes, checksum_offset: int, checksum_size: int) -> bool:
    stored = struct.unpack_from("<Q", block, checksum_offset)[0]
    if any(block[checksum_offset + 8:checksum_offset + checksum_size]):
        return False
    scratch = bytearray(block)
    scratch[checksum_offset:checksum_offset + checksum_size] = bytes(checksum_size)
    return stored == crc64(scratch)


def read_allocation_page(image, physical: int, magic: bytes,
                         max_generation: int, logical: int, level: int,
                         expected_entries: int) -> bytearray:
    page = read_block(image, physical)
    if page[:8] != magic:
        raise ValueError(f"allocation page {physical} has wrong magic")
    generation, page_logical = struct.unpack_from("<QQ", page, 8)
    page_level, entries, bytes_used, checksum_type = struct.unpack_from(
        "<IIII", page, 24)
    if not generation or generation > max_generation:
        raise ValueError(f"allocation page {physical} generation is invalid")
    if page_logical != logical or page_level != level or entries != expected_entries:
        raise ValueError(f"allocation page {physical} shape is invalid")
    if checksum_type != CHECKSUM_CRC64_ECMA:
        raise ValueError(f"allocation page {physical} checksum type is invalid")
    expected_bytes = ceil_div(entries, 8) if level == 0 else entries * 8
    if bytes_used != expected_bytes or bytes_used > ALLOCATION_DATA_SIZE:
        raise ValueError(f"allocation page {physical} payload size is invalid")
    if any(page[ALLOCATION_HEADER_SIZE + bytes_used:]):
        raise ValueError(f"allocation page {physical} trailing bytes are not zero")
    if not validate_crc(page, ALLOCATION_CHECKSUM_OFFSET,
                        ALLOCATION_CHECKSUM_SIZE):
        raise ValueError(f"allocation page {physical} checksum is invalid")
    return page


def load_allocation_tree(image, checkpoint: bytearray):
    generation = struct.unpack_from("<Q", checkpoint, 20)[0]
    total_blocks = struct.unpack_from("<Q", checkpoint, 28)[0]
    root = struct.unpack_from("<Q", checkpoint, 44)[0]
    leaf_count = struct.unpack_from("<Q", checkpoint, 52)[0]
    expected_leaves = ceil_div(total_blocks, ALLOCATION_BITS_PER_LEAF)
    if leaf_count != expected_leaves:
        raise ValueError("checkpoint allocation leaf count is invalid")

    level1_count = ceil_div(leaf_count, ALLOCATION_FANOUT)
    level2_count = ceil_div(level1_count, ALLOCATION_FANOUT)
    bitmap = bytearray(ceil_div(total_blocks, 8))
    pages = []

    root_page = read_allocation_page(
        image, root, b"INFSAB01", generation, 0,
        ALLOCATION_ROOT_LEVEL, level2_count)
    pages.append(root)
    level2_ptrs = struct.unpack_from(
        f"<{level2_count}Q", root_page, ALLOCATION_HEADER_SIZE)

    global_level1 = 0
    global_leaf = 0
    for level2_index, level2_block in enumerate(level2_ptrs):
        entries = min(ALLOCATION_FANOUT, level1_count - global_level1)
        level2_page = read_allocation_page(
            image, level2_block, b"INFSAB01", generation,
            level2_index, 2, entries)
        pages.append(level2_block)
        level1_ptrs = struct.unpack_from(
            f"<{entries}Q", level2_page, ALLOCATION_HEADER_SIZE)

        for level1_block in level1_ptrs:
            leaf_entries = min(ALLOCATION_FANOUT, leaf_count - global_leaf)
            level1_page = read_allocation_page(
                image, level1_block, b"INFSAB01", generation,
                global_level1, 1, leaf_entries)
            pages.append(level1_block)
            leaf_ptrs = struct.unpack_from(
                f"<{leaf_entries}Q", level1_page, ALLOCATION_HEADER_SIZE)

            for leaf_block in leaf_ptrs:
                first = global_leaf * ALLOCATION_BITS_PER_LEAF
                valid_bits = min(ALLOCATION_BITS_PER_LEAF, total_blocks - first)
                leaf_page = read_allocation_page(
                    image, leaf_block, b"INFSAL01", generation,
                    global_leaf, 0, valid_bits)
                pages.append(leaf_block)
                bytes_used = ceil_div(valid_bits, 8)
                destination = global_leaf * ALLOCATION_DATA_SIZE
                chunk = leaf_page[
                    ALLOCATION_HEADER_SIZE:ALLOCATION_HEADER_SIZE + bytes_used]
                bitmap[destination:destination + bytes_used] = chunk
                global_leaf += 1
            global_level1 += 1

    if global_leaf != leaf_count or global_level1 != level1_count:
        raise ValueError("allocation tree traversal did not cover declared geometry")
    return bitmap, pages, leaf_count, level1_count, level2_count


def make_allocation_page(magic: bytes, generation: int, logical: int,
                         level: int, entries: int, payload: bytes) -> bytes:
    if len(magic) != 8 or not entries:
        raise ValueError("invalid allocation page arguments")
    expected = ceil_div(entries, 8) if level == 0 else entries * 8
    if len(payload) != expected or expected > ALLOCATION_DATA_SIZE:
        raise ValueError("invalid allocation page payload")

    block = bytearray(BLOCK_SIZE)
    struct.pack_into(
        "<8sQQIIII", block, 0, magic, generation, logical, level,
        entries, len(payload), CHECKSUM_CRC64_ECMA)
    block[ALLOCATION_HEADER_SIZE:ALLOCATION_HEADER_SIZE + len(payload)] = payload
    struct.pack_into(
        "<Q", block, ALLOCATION_CHECKSUM_OFFSET, crc64(block))
    return bytes(block)


def find_non_root_index_leaf(image, checkpoint: bytearray) -> int:
    index_block = struct.unpack_from("<Q", checkpoint, 60)[0]
    root_object_id = bytes(checkpoint[116:132])
    index = read_block(image, index_block)
    if (index[:8] != b"INFOBJ01" or
            struct.unpack_from("<H", index, 8)[0] != 3 or
            struct.unpack_from("<H", index, 10)[0] != 3):
        raise ValueError("current object-index head is invalid")
    tree_root = struct.unpack_from("<Q", index, 104)[0]
    pending = [tree_root]
    visited = set()
    while pending:
        node = pending.pop()
        if not node or node in visited:
            raise ValueError("object-index tree contains an invalid cycle")
        visited.add(node)
        page = read_block(image, node)
        if page[:8] == b"INFSIP01":
            count = struct.unpack_from("<I", page, 32)[0]
            ids = [page[80 + idx * 32:96 + idx * 32]
                   for idx in range(count)]
            if root_object_id not in ids:
                return node
            continue
        if page[:8] != b"INFSIB01":
            raise ValueError("object-index tree page magic is invalid")
        children = struct.unpack_from("<256Q", page, 80)
        pending.extend(child for child in children if child)
    raise ValueError("object-index tree has no non-root leaf to corrupt")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} IMAGE", file=sys.stderr)
        return 2
    path = sys.argv[1]

    with open(path, "r+b", buffering=0) as image:
        checkpoint = bytearray(read_block(image, 0))
        if checkpoint[:8] != b"INFS2026":
            raise ValueError("primary checkpoint magic is invalid")
        if not validate_crc(checkpoint, CHECKSUM_OFFSET, CHECKSUM_SIZE):
            raise ValueError("primary checkpoint checksum is invalid")

        generation = struct.unpack_from("<Q", checkpoint, 20)[0]
        total_blocks = struct.unpack_from("<Q", checkpoint, 28)[0]
        checkpoint_blocks = struct.unpack_from("<QQQ", checkpoint, 76)
        bitmap, _, leaf_count, level1_count, level2_count = \
            load_allocation_tree(image, checkpoint)

        freed_child = find_non_root_index_leaf(image, checkpoint)
        if not 0 < freed_child < total_blocks or not bit_get(bitmap, freed_child):
            raise ValueError("tree child is not allocated by selected generation")

        replacement_count = leaf_count + level1_count + level2_count + 1
        replacements = []
        for block in range(1, total_blocks):
            if block in checkpoint_blocks or bit_get(bitmap, block):
                continue
            replacements.append(block)
            if len(replacements) == replacement_count:
                break
        if len(replacements) != replacement_count:
            raise ValueError("not enough free blocks for replacement allocation tree")

        new_leaves = replacements[:leaf_count]
        branch_at = leaf_count
        new_root = replacements[branch_at]
        branch_at += 1
        new_level2 = replacements[branch_at:branch_at + level2_count]
        branch_at += level2_count
        new_level1 = replacements[branch_at:branch_at + level1_count]

        for block in replacements:
            bit_set(bitmap, block, True)
        bit_set(bitmap, freed_child, False)

        next_generation = generation + 1
        for leaf_index, physical in enumerate(new_leaves):
            first = leaf_index * ALLOCATION_BITS_PER_LEAF
            valid_bits = min(ALLOCATION_BITS_PER_LEAF, total_blocks - first)
            bytes_used = ceil_div(valid_bits, 8)
            source = leaf_index * ALLOCATION_DATA_SIZE
            payload = bytes(bitmap[source:source + bytes_used])
            write_block(image, physical, make_allocation_page(
                b"INFSAL01", next_generation, leaf_index, 0,
                valid_bits, payload))

        for index, physical in enumerate(new_level1):
            first_leaf = index * ALLOCATION_FANOUT
            children = new_leaves[
                first_leaf:first_leaf + ALLOCATION_FANOUT]
            payload = struct.pack(f"<{len(children)}Q", *children)
            write_block(image, physical, make_allocation_page(
                b"INFSAB01", next_generation, index, 1,
                len(children), payload))

        for index, physical in enumerate(new_level2):
            first_branch = index * ALLOCATION_FANOUT
            children = new_level1[
                first_branch:first_branch + ALLOCATION_FANOUT]
            payload = struct.pack(f"<{len(children)}Q", *children)
            write_block(image, physical, make_allocation_page(
                b"INFSAB01", next_generation, index, 2,
                len(children), payload))

        root_payload = struct.pack(
            f"<{len(new_level2)}Q", *new_level2)
        write_block(image, new_root, make_allocation_page(
            b"INFSAB01", next_generation, 0, ALLOCATION_ROOT_LEVEL,
            len(new_level2), root_payload))

        free_blocks = sum(
            not bit_get(bitmap, block) for block in range(total_blocks))
        struct.pack_into("<Q", checkpoint, 20, next_generation)
        struct.pack_into("<Q", checkpoint, 36, free_blocks)
        struct.pack_into("<Q", checkpoint, 44, new_root)
        struct.pack_into("<Q", checkpoint, 52, leaf_count)
        checkpoint[CHECKSUM_OFFSET:CHECKSUM_OFFSET + CHECKSUM_SIZE] = \
            bytes(CHECKSUM_SIZE)
        struct.pack_into("<Q", checkpoint, CHECKSUM_OFFSET, crc64(checkpoint))
        write_block(image, 0, checkpoint)
        image.flush()
        os.fsync(image.fileno())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
