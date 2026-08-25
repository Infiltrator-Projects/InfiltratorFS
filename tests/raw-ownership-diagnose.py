#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only raw diagnostic for a published Format 0.12 image.

This intentionally does not use the InfiltratorFS opener: it reconstructs the
live ownership set from the checkpoint, bitmap, index, directory metadata,
file extent pages and normal data extents. It is used only by regression tests
to explain why a strict graph open rejected an otherwise parseable image.
"""
import struct
import sys

BLOCK = 4096
OBJ_HDR = 96
META_HDR = 80
TYPE_DIR = 1
TYPE_FILE = 2
TYPE_INDEX = 3
TYPE_CHECKSUM = 4
TYPE_SYMLINK = 5
TYPE_SNAPSHOT = 6
VERSION_PAGED = 2
EXTENT_NORMAL = 0


def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u64(b, o): return struct.unpack_from('<Q', b, o)[0]


def main(path):
    with open(path, 'rb') as f:
        sb = f.read(BLOCK)
        total = u64(sb, 28)
        bitmap_start = u64(sb, 44)
        bitmap_blocks = u64(sb, 52)
        index_block = u64(sb, 60)
        checkpoints = [u64(sb, 76 + 8*i) for i in range(3)]
        f.seek(bitmap_start * BLOCK)
        bitmap = f.read(bitmap_blocks * BLOCK)

        def read_block(n):
            f.seek(n * BLOCK)
            return f.read(BLOCK)

        def allocated(n):
            return 0 <= n < total and ((bitmap[n >> 3] >> (n & 7)) & 1) != 0

        owners = set(checkpoints)
        owners.update(range(bitmap_start, bitmap_start + bitmap_blocks))
        owners.add(index_block)

        index = read_block(index_block)
        index_version = u16(index, 10)
        index_count = u32(index, OBJ_HDR)
        index_pages = u32(index, OBJ_HDR + 4)
        entries = []
        if index_version == VERSION_PAGED:
            for pi in range(index_pages):
                pb = u64(index, OBJ_HDR + 8 + 8*pi)
                owners.add(pb)
                page = read_block(pb)
                count = u32(page, 32)
                for i in range(count):
                    off = META_HDR + i * 32
                    oid = page[off:off+16]
                    ob = u64(page, off + 16)
                    typ = u16(page, off + 24)
                    entries.append((oid, ob, typ))
        else:
            for i in range(index_count):
                off = OBJ_HDR + 8 + i * 32
                oid = index[off:off+16]
                ob = u64(index, off + 16)
                typ = u16(index, off + 24)
                entries.append((oid, ob, typ))

        print(f'DIAG_INDEX entries_declared={index_count} entries_parsed={len(entries)} pages={index_pages}')
        type_counts = {}
        for _, ob, typ in entries:
            owners.add(ob)
            type_counts[typ] = type_counts.get(typ, 0) + 1
            obj = read_block(ob)
            version = u16(obj, 10)
            if typ == TYPE_DIR and version == VERSION_PAGED:
                page_count = u32(obj, OBJ_HDR + 108)
                for pi in range(page_count):
                    owners.add(u64(obj, OBJ_HDR + 112 + 8*pi))
            elif typ == TYPE_FILE:
                extent_count = u32(obj, OBJ_HDR + 104)
                if extent_count == 0:
                    continue
                if version == VERSION_PAGED:
                    page_count = u32(obj, OBJ_HDR + 128)
                    extent_pages = []
                    for pi in range(page_count):
                        pb = u64(obj, OBJ_HDR + 136 + 8*pi)
                        owners.add(pb)
                        extent_pages.append(read_block(pb))
                    extent_vectors = []
                    for page in extent_pages:
                        count = u32(page, 32)
                        extent_vectors.extend(
                            struct.unpack_from('<QQII', page, META_HDR + i*24)
                            for i in range(count))
                else:
                    extent_vectors = [
                        struct.unpack_from('<QQII', obj, OBJ_HDR + 128 + i*24)
                        for i in range(extent_count)
                    ]
                for logical, physical, blocks, flags in extent_vectors:
                    if flags == EXTENT_NORMAL:
                        owners.update(range(physical, physical + blocks))

        allocated_set = {b for b in range(total) if allocated(b)}
        missing = sorted(owners - allocated_set)
        extra = sorted(allocated_set - owners)
        print('DIAG_INDEX_TYPES ' + ' '.join(f'type{t}={n}' for t, n in sorted(type_counts.items())))
        print(f'DIAG_OWNERSHIP allocated={len(allocated_set)} claimed={len(owners)} missing={len(missing)} extra={len(extra)}')
        if missing:
            print('DIAG_OWNERSHIP_MISSING ' + ' '.join(map(str, missing[:32])))
        if extra:
            print('DIAG_OWNERSHIP_EXTRA ' + ' '.join(map(str, extra[:64])))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: raw-ownership-diagnose.py <image>')
    main(sys.argv[1])
