#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Publish one higher checkpoint whose bitmap frees a live tree child."""

import os
import struct
import sys


BLOCK_SIZE = 4096
CHECKSUM_OFFSET = 220
CHECKSUM_SIZE = 32
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


def bit_get(bitmap: bytearray, block: int) -> bool:
    return bool(bitmap[block >> 3] & (1 << (block & 7)))


def bit_set(bitmap: bytearray, block: int, used: bool) -> None:
    mask = 1 << (block & 7)
    if used:
        bitmap[block >> 3] |= mask
    else:
        bitmap[block >> 3] &= ~mask


def find_non_root_index_leaf(image, checkpoint: bytearray) -> int:
    index_block = struct.unpack_from("<Q", checkpoint, 60)[0]
    root_object_id = bytes(checkpoint[116:132])
    image.seek(index_block * BLOCK_SIZE)
    index = image.read(BLOCK_SIZE)
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
        image.seek(node * BLOCK_SIZE)
        page = image.read(BLOCK_SIZE)
        if page[:8] == b"INFSIP01":
            count = struct.unpack_from("<I", page, 32)[0]
            ids = [page[80 + index * 32:96 + index * 32]
                   for index in range(count)]
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
        checkpoint = bytearray(image.read(BLOCK_SIZE))
        if checkpoint[:8] != b"INFS2026":
            raise ValueError("primary checkpoint magic is invalid")
        generation = struct.unpack_from("<Q", checkpoint, 20)[0]
        total_blocks = struct.unpack_from("<Q", checkpoint, 28)[0]
        bitmap_start = struct.unpack_from("<Q", checkpoint, 44)[0]
        bitmap_blocks = struct.unpack_from("<Q", checkpoint, 52)[0]
        checkpoint_blocks = struct.unpack_from("<QQQ", checkpoint, 76)
        freed_child = find_non_root_index_leaf(image, checkpoint)
        if not 0 < freed_child < total_blocks:
            raise ValueError("tree child is outside the volume")

        image.seek(bitmap_start * BLOCK_SIZE)
        bitmap = bytearray(image.read(bitmap_blocks * BLOCK_SIZE))
        if len(bitmap) != bitmap_blocks * BLOCK_SIZE or not bit_get(bitmap, freed_child):
            raise ValueError("tree child is not allocated by the selected bitmap")

        replacement = None
        for start in range(1, total_blocks - bitmap_blocks + 1):
            if any(start <= checkpoint_block < start + bitmap_blocks
                   for checkpoint_block in checkpoint_blocks):
                continue
            if all(not bit_get(bitmap, start + offset)
                   for offset in range(bitmap_blocks)):
                replacement = start
                break
        if replacement is None:
            raise ValueError("no free run is available for a replacement bitmap")

        for offset in range(bitmap_blocks):
            bit_set(bitmap, replacement + offset, True)
        bit_set(bitmap, freed_child, False)
        free_blocks = sum(not bit_get(bitmap, block)
                          for block in range(total_blocks))

        image.seek(replacement * BLOCK_SIZE)
        image.write(bitmap)
        struct.pack_into("<Q", checkpoint, 20, generation + 1)
        struct.pack_into("<Q", checkpoint, 36, free_blocks)
        struct.pack_into("<Q", checkpoint, 44, replacement)
        checkpoint[CHECKSUM_OFFSET:CHECKSUM_OFFSET + CHECKSUM_SIZE] = \
            bytes(CHECKSUM_SIZE)
        struct.pack_into("<Q", checkpoint, CHECKSUM_OFFSET, crc64(checkpoint))
        image.seek(0)
        image.write(checkpoint)
        image.flush()
        os.fsync(image.fileno())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
