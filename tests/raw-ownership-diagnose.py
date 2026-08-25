#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only raw diagnostic for a published Format 0.12 image.

This intentionally does not use the InfiltratorFS opener. It reconstructs the
live ownership set and the file checksum chain directly from published bytes so
mounted regressions can explain a strict graph-open rejection without relying
on the graph opener that is under diagnosis.
"""
import struct
import sys

BLOCK = 4096
OBJ_HDR = 96
META_HDR = 80
TYPE_DIR = 1
TYPE_FILE = 2
TYPE_CHECKSUM = 4
VERSION_PAGED = 2
EXTENT_NORMAL = 0
CHECKSUMS_PER_OBJECT = (BLOCK - OBJ_HDR - 48) // 32
ZERO_ID = bytes(16)


def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u64(b, o): return struct.unpack_from('<Q', b, o)[0]
def idtext(v): return v.hex()


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

        owner_sources = {}
        duplicate_claims = []

        def claim(block, source):
            previous = owner_sources.get(block)
            if previous is not None:
                duplicate_claims.append((block, previous, source))
            else:
                owner_sources[block] = source

        for i, block in enumerate(checkpoints):
            claim(block, f'checkpoint[{i}]')
        for block in range(bitmap_start, bitmap_start + bitmap_blocks):
            claim(block, 'bitmap')
        claim(index_block, 'index-head')

        index = read_block(index_block)
        index_version = u16(index, 10)
        index_count = u32(index, OBJ_HDR)
        index_pages = u32(index, OBJ_HDR + 4)
        entries = []
        if index_version == VERSION_PAGED:
            for pi in range(index_pages):
                pb = u64(index, OBJ_HDR + 8 + 8*pi)
                claim(pb, f'index-page[{pi}]')
                page = read_block(pb)
                count = u32(page, 32)
                for i in range(count):
                    off = META_HDR + i * 32
                    entries.append((page[off:off+16], u64(page, off + 16),
                                    u16(page, off + 24)))
        else:
            for i in range(index_count):
                off = OBJ_HDR + 8 + i * 32
                entries.append((index[off:off+16], u64(index, off + 16),
                                u16(index, off + 24)))

        print(f'DIAG_INDEX entries_declared={index_count} entries_parsed={len(entries)} pages={index_pages}')
        type_counts = {}
        file_details = []
        for ei, (oid, ob, typ) in enumerate(entries):
            claim(ob, f'object[{ei}]-type{typ}-{idtext(oid)}')
            type_counts[typ] = type_counts.get(typ, 0) + 1
            obj = read_block(ob)
            version = u16(obj, 10)
            if typ == TYPE_DIR and version == VERSION_PAGED:
                page_count = u32(obj, OBJ_HDR + 108)
                for pi in range(page_count):
                    claim(u64(obj, OBJ_HDR + 112 + 8*pi),
                          f'dir-page[{ei}:{pi}]-{idtext(oid)}')
            elif typ == TYPE_FILE:
                extent_count = u32(obj, OBJ_HDR + 104)
                checksum_head = obj[OBJ_HDR + 112:OBJ_HDR + 128]
                extent_vectors = []
                if extent_count:
                    if version == VERSION_PAGED:
                        page_count = u32(obj, OBJ_HDR + 128)
                        for pi in range(page_count):
                            pb = u64(obj, OBJ_HDR + 136 + 8*pi)
                            claim(pb, f'extent-page[{ei}:{pi}]-{idtext(oid)}')
                            page = read_block(pb)
                            count = u32(page, 32)
                            extent_vectors.extend(
                                struct.unpack_from('<QQII', page, META_HDR + i*24)
                                for i in range(count))
                    else:
                        extent_vectors = [
                            struct.unpack_from('<QQII', obj, OBJ_HDR + 128 + i*24)
                            for i in range(extent_count)
                        ]
                for xi, (logical, physical, blocks, flags) in enumerate(extent_vectors):
                    if flags == EXTENT_NORMAL:
                        for block in range(physical, physical + blocks):
                            claim(block, f'data[{ei}:{xi}]-logical{logical}-{idtext(oid)}')
                file_details.append((oid, ob, checksum_head, extent_vectors))

        owners = set(owner_sources)
        allocated_set = {b for b in range(total) if allocated(b)}
        missing = sorted(owners - allocated_set)
        extra = sorted(allocated_set - owners)
        print('DIAG_INDEX_TYPES ' + ' '.join(f'type{t}={n}' for t, n in sorted(type_counts.items())))
        print(f'DIAG_OWNERSHIP allocated={len(allocated_set)} claimed={len(owners)} missing={len(missing)} extra={len(extra)} duplicates={len(duplicate_claims)}')
        if missing:
            print('DIAG_OWNERSHIP_MISSING ' + ' '.join(map(str, missing[:32])))
        if extra:
            print('DIAG_OWNERSHIP_EXTRA ' + ' '.join(map(str, extra[:64])))
        for block, first, second in duplicate_claims[:32]:
            print(f'DIAG_OWNERSHIP_DUPLICATE block={block} first={first} second={second}')

        index_by_id = {oid: (ob, typ) for oid, ob, typ in entries}
        indexed_checksum_ids = {oid for oid, _, typ in entries if typ == TYPE_CHECKSUM}
        globally_seen = set()
        for file_id, file_block, head_id, extents in file_details:
            chain = []
            local_seen = set()
            current = head_id
            order_bad = owner_bad = parent_bad = id_bad = count_bad = 0
            previous = None
            segment_counts = {}
            while current != ZERO_ID:
                if current in local_seen:
                    print(f'DIAG_CHECKSUM_CYCLE file={file_block} id={idtext(current)}')
                    break
                local_seen.add(current)
                entry = index_by_id.get(current)
                if not entry or entry[1] != TYPE_CHECKSUM:
                    print(f'DIAG_CHECKSUM_MISSING_INDEX file={file_block} id={idtext(current)}')
                    break
                ob = entry[0]
                obj = read_block(ob)
                object_id = obj[24:40]
                parent_id = obj[40:56]
                owner_id = obj[OBJ_HDR:OBJ_HDR+16]
                next_id = obj[OBJ_HDR+16:OBJ_HDR+32]
                start = u64(obj, OBJ_HDR+32)
                checksum_count = u32(obj, OBJ_HDR+40)
                if object_id != current: id_bad += 1
                if parent_id != file_id: parent_bad += 1
                if owner_id != file_id: owner_bad += 1
                if previous is not None and start <= previous: order_bad += 1
                if start % CHECKSUMS_PER_OBJECT != 0 or checksum_count > CHECKSUMS_PER_OBJECT:
                    count_bad += 1
                chain.append((current, ob, start, checksum_count))
                segment_counts[start] = checksum_count
                globally_seen.add(current)
                previous = start
                current = next_id
            uncovered = []
            for logical, _, blocks, flags in extents:
                if flags != EXTENT_NORMAL:
                    continue
                for lb in range(logical, logical + blocks):
                    start = (lb // CHECKSUMS_PER_OBJECT) * CHECKSUMS_PER_OBJECT
                    if segment_counts.get(start, 0) <= lb - start:
                        uncovered.append(lb)
                        if len(uncovered) >= 16:
                            break
                if len(uncovered) >= 16:
                    break
            starts = [x[2] for x in chain]
            print('DIAG_CHECKSUM_CHAIN '
                  f'file={file_block} nodes={len(chain)} head={idtext(head_id)} '
                  f'order_bad={order_bad} owner_bad={owner_bad} parent_bad={parent_bad} '
                  f'id_bad={id_bad} count_bad={count_bad} uncovered={len(uncovered)} '
                  f'min_start={min(starts) if starts else -1} max_start={max(starts) if starts else -1}')
            if uncovered:
                print('DIAG_CHECKSUM_UNCOVERED ' + ' '.join(map(str, uncovered)))

        unreachable = indexed_checksum_ids - globally_seen
        print(f'DIAG_CHECKSUM_INDEX indexed={len(indexed_checksum_ids)} reached={len(globally_seen)} unreachable={len(unreachable)}')
        if unreachable:
            print('DIAG_CHECKSUM_UNREACHABLE ' + ' '.join(idtext(x) for x in list(unreachable)[:16]))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: raw-ownership-diagnose.py <image>')
    main(sys.argv[1])
