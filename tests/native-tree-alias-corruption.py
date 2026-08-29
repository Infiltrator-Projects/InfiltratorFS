#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Create checksummed, heavily aliased directory metadata for mount hardening.

All checkpoint replicas remain byte-for-byte identical and valid.  Only deeper
metadata is changed: the root directory points at a three-level DAG whose 256
branch slots repeatedly reference the same child blocks.  An unsafe recursive
walker revisits the same leaf more than sixteen million times; a hardened
walker must reject the first repeated physical block as corruption.
"""

import os
import struct
import sys

BLOCK_SIZE = 4096
CHECKSUM_CRC64_ECMA = 1
POLYNOMIAL = 0x42F0E1EBA9EA3693

SUPER_INDEX_BLOCK = 60
SUPER_ROOT_BLOCK = 68
SUPER_CHECKPOINTS = 76
SUPER_ROOT_ID = 116

OBJECT_GENERATION = 16
OBJECT_PAYLOAD_SIZE = 56
OBJECT_HEADER_SIZE = 96
DIRECTORY_PAYLOAD_SIZE = 112
DIRECTORY_ENTRY_COUNT = OBJECT_HEADER_SIZE + 104
DIRECTORY_BYTES_USED = OBJECT_HEADER_SIZE + 108
DIRECTORY_ROOT = OBJECT_HEADER_SIZE + DIRECTORY_PAYLOAD_SIZE

INDEX_TREE_ROOT = OBJECT_HEADER_SIZE + 8
PAGE_HEADER_SIZE = 80
PAGE_CHECKSUM_OFFSET = 48
PAGE_CHECKSUM_SIZE = 32
TREE_FANOUT = 256
TREE_BRANCH_BYTES = TREE_FANOUT * 8
INDEX_ENTRY_SIZE = 32
INDEX_ENTRY_BLOCK = 16
INDEX_ENTRY_TYPE = 24


def crc64(block: bytes) -> int:
    crc = 0
    for byte in block:
        crc ^= byte << 56
        for _ in range(8):
            crc = (((crc << 1) ^ POLYNOMIAL) & 0xFFFFFFFFFFFFFFFF
                   if crc & (1 << 63)
                   else (crc << 1) & 0xFFFFFFFFFFFFFFFF)
    return crc


def read_block(image, block: int) -> bytearray:
    image.seek(block * BLOCK_SIZE)
    data = bytearray(image.read(BLOCK_SIZE))
    if len(data) != BLOCK_SIZE:
        raise ValueError(f"short read at block {block}")
    return data


def write_block(image, block: int, data: bytes) -> None:
    if len(data) != BLOCK_SIZE:
        raise ValueError("metadata page must be exactly one block")
    image.seek(block * BLOCK_SIZE)
    image.write(data)


def finish_page(block: bytearray) -> bytes:
    block[PAGE_CHECKSUM_OFFSET:PAGE_CHECKSUM_OFFSET + PAGE_CHECKSUM_SIZE] = (
        bytes(PAGE_CHECKSUM_SIZE)
    )
    struct.pack_into("<Q", block, PAGE_CHECKSUM_OFFSET, crc64(block))
    return bytes(block)


def branch_page(generation: int, owner: bytes, child: int) -> bytes:
    block = bytearray(BLOCK_SIZE)
    struct.pack_into(
        "<8sQ16sIIII", block, 0, b"INFSDB01", generation, owner,
        TREE_FANOUT, TREE_BRANCH_BYTES, CHECKSUM_CRC64_ECMA, 0
    )
    for slot in range(TREE_FANOUT):
        struct.pack_into("<Q", block, PAGE_HEADER_SIZE + slot * 8, child)
    return finish_page(block)


def empty_leaf_page(generation: int, owner: bytes) -> bytes:
    block = bytearray(BLOCK_SIZE)
    struct.pack_into(
        "<8sQ16sIIII", block, 0, b"INFSDP01", generation, owner,
        0, 0, CHECKSUM_CRC64_ECMA, 0
    )
    return finish_page(block)


def file_object_blocks(image, index_block: int, excluded: set[int],
                       wanted: int) -> list[int]:
    index = read_block(image, index_block)
    if index[:8] != b"INFOBJ01":
        raise ValueError("object index has invalid magic")
    obj_type, version = struct.unpack_from("<HH", index, 8)
    if obj_type != 3 or version != 3:
        raise ValueError("object index is not the Format 0.17 radix tree")
    root = struct.unpack_from("<Q", index, INDEX_TREE_ROOT)[0]
    if not root:
        raise ValueError("object index tree root is zero")

    pending = [root]
    visited = set()
    found: list[int] = []
    while pending and len(found) < wanted:
        node = pending.pop()
        if not node or node in visited:
            raise ValueError("object index contains a cycle or alias")
        visited.add(node)
        page = read_block(image, node)
        if page[:8] == b"INFSIB01":
            children = struct.unpack_from(
                f"<{TREE_FANOUT}Q", page, PAGE_HEADER_SIZE
            )
            pending.extend(child for child in children if child)
            continue
        if page[:8] != b"INFSIP01":
            raise ValueError(f"unexpected index-tree page at block {node}")
        count, bytes_used = struct.unpack_from("<II", page, 32)
        if bytes_used != count * INDEX_ENTRY_SIZE:
            raise ValueError("invalid index leaf size")
        for entry_index in range(count):
            offset = PAGE_HEADER_SIZE + entry_index * INDEX_ENTRY_SIZE
            object_block = struct.unpack_from(
                "<Q", page, offset + INDEX_ENTRY_BLOCK
            )[0]
            object_type = struct.unpack_from(
                "<H", page, offset + INDEX_ENTRY_TYPE
            )[0]
            if (object_type != 2 or object_block in excluded or not object_block):
                continue
            if object_block not in found:
                found.append(object_block)
                if len(found) == wanted:
                    break
    if len(found) != wanted:
        raise ValueError(f"need {wanted} ordinary file-object blocks, found {len(found)}")
    return found


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} IMAGE", file=sys.stderr)
        return 2
    path = sys.argv[1]

    with open(path, "r+b", buffering=0) as image:
        checkpoint = read_block(image, 0)
        if checkpoint[:8] != b"INFS2026":
            raise ValueError("primary checkpoint has invalid magic")
        total_blocks = os.path.getsize(path) // BLOCK_SIZE
        checkpoints = struct.unpack_from("<QQQ", checkpoint, SUPER_CHECKPOINTS)
        copies = [checkpoint]
        for location in checkpoints[1:]:
            copies.append(read_block(image, location))
        if not copies[0] == copies[1] == copies[2]:
            raise ValueError("test requires three agreeing checkpoint replicas")

        index_block = struct.unpack_from("<Q", checkpoint, SUPER_INDEX_BLOCK)[0]
        root_object_block = struct.unpack_from(
            "<Q", checkpoint, SUPER_ROOT_BLOCK
        )[0]
        root_id = bytes(checkpoint[SUPER_ROOT_ID:SUPER_ROOT_ID + 16])
        if not root_object_block or root_object_block >= total_blocks:
            raise ValueError("root object block is invalid")

        root_object = read_block(image, root_object_block)
        if root_object[:8] != b"INFOBJ01":
            raise ValueError("root object has invalid magic")
        obj_type, version = struct.unpack_from("<HH", root_object, 8)
        if obj_type != 1 or version != 3:
            raise ValueError("root directory is not tree-backed")
        generation = struct.unpack_from("<Q", root_object, OBJECT_GENERATION)[0]
        payload_size = struct.unpack_from(
            "<I", root_object, OBJECT_PAYLOAD_SIZE
        )[0]
        entry_count = struct.unpack_from(
            "<I", root_object, DIRECTORY_ENTRY_COUNT
        )[0]
        bytes_used = struct.unpack_from(
            "<I", root_object, DIRECTORY_BYTES_USED
        )[0]
        root_tree = struct.unpack_from("<Q", root_object, DIRECTORY_ROOT)[0]
        if payload_size != DIRECTORY_PAYLOAD_SIZE + 8:
            raise ValueError("root directory tree payload size is invalid")
        if not entry_count or bytes_used != 0 or not root_tree:
            raise ValueError("root directory is unsuitable for alias test")

        excluded = set(checkpoints)
        excluded.update((index_block, root_object_block, root_tree))
        branch_one, branch_two, leaf = file_object_blocks(
            image, index_block, excluded, 3
        )

        write_block(image, leaf, empty_leaf_page(generation, root_id))
        write_block(image, branch_two,
                    branch_page(generation, root_id, leaf))
        write_block(image, branch_one,
                    branch_page(generation, root_id, branch_two))
        write_block(image, root_tree,
                    branch_page(generation, root_id, branch_one))
        image.flush()
        os.fsync(image.fileno())

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
