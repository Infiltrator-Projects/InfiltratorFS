#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail

build_dir="${1:-build}"
mkfs="$build_dir/mkfs.infilfs"
scrub="$build_dir/infilfs-scrub"
inspect="$build_dir/infilfs-inspect"
forensic="$build_dir/infilfs-forensic"
fuse="$build_dir/infilfs-fuse"

tmp="$(mktemp -d)"
image="$tmp/minimal.img"
mnt="$tmp/mnt"
log="$tmp/fuse.log"
forensic_json="$tmp/forensic.jsonl"
pid=""

cleanup() {
    set +e
    if mountpoint -q "$mnt" 2>/dev/null; then
        fusermount3 -u "$mnt" 2>/dev/null || fusermount3 -uz "$mnt" 2>/dev/null || true
    fi
    [[ -n "$pid" ]] && wait "$pid" 2>/dev/null || true
    rm -rf "$tmp"
}
trap cleanup EXIT

truncate -s 1G "$image"
"$mkfs" -L MinimalPagedPromotion "$image" >/dev/null
mkdir -p "$mnt"
"$fuse" "$image" "$mnt" -f >"$log" 2>&1 &
pid=$!
for _ in $(seq 1 100); do
    mountpoint -q "$mnt" && break
    kill -0 "$pid" 2>/dev/null || { cat "$log" >&2; exit 1; }
    sleep 0.1
done
mountpoint -q "$mnt"

python3 - "$mnt" <<'PY'
import os, random, sys
path = os.path.join(sys.argv[1], 'random-update.bin')
size = 512 * 1024 * 1024
with open(path, 'wb') as f:
    f.truncate(size)
rng = random.Random(0x1F11F5)
fd = os.open(path, os.O_RDWR)
try:
    block = bytearray(4096)
    for i in range(81):
        logical = rng.randrange(0, size // 4096)
        block[0:8] = i.to_bytes(8, 'little')
        n = os.pwrite(fd, block, logical * 4096)
        if n != 4096:
            raise RuntimeError(f'short pwrite {n} at i={i}')
        if i in (79, 80):
            print(f'PROMOTION_PWRITE_OK i={i} logical={logical}', flush=True)
    os.fsync(fd)
    print('PROMOTION_FSYNC_OK i=80', flush=True)
finally:
    os.close(fd)
PY

fusermount3 -u "$mnt"
wait "$pid" 2>/dev/null || true
pid=""

"$inspect" "$image"
"$forensic" --jsonl "$image" >"$forensic_json"
python3 - "$image" "$forensic_json" <<'PY'
import json, struct, sys
image, report = sys.argv[1:3]
records = []
with open(report, 'r', encoding='utf-8') as f:
    for line in f:
        record = json.loads(line)
        if record.get('record') == 'metadata':
            records.append(record)
files = [r for r in records if r.get('object_type') == 'file']
print(f'DIAG_FILE_RECORDS count={len(files)}')
for r in files:
    print('DIAG_FILE_RECORD '
          f"block={r['block']} state={r['state']} generation={r['generation']} "
          f"version={r['object_version']} payload={r['payload_size']}")
paged = [r for r in files if r.get('object_version') == 2]
if not paged:
    print('DIAG_NO_PAGED_FILE_RECORD')
    raise SystemExit(0)
# Highest-generation paged file is the promoted head. The scanner marks state
# unknown when strict open fails, so allocation state is established directly
# from the published bitmap below rather than from forensic classification.
r = max(paged, key=lambda x: (int(x['generation']), int(x['block'])))
file_block = int(r['block'])
with open(image, 'rb') as f:
    sb = f.read(4096)
    checkpoint_generation = struct.unpack_from('<Q', sb, 20)[0]
    total_blocks = struct.unpack_from('<Q', sb, 28)[0]
    bitmap_start = struct.unpack_from('<Q', sb, 44)[0]
    bitmap_blocks = struct.unpack_from('<Q', sb, 52)[0]
    f.seek(bitmap_start * 4096)
    bitmap = f.read(bitmap_blocks * 4096)
    f.seek(file_block * 4096)
    obj = f.read(4096)

def allocated(block):
    return 0 <= block < total_blocks and ((bitmap[block >> 3] >> (block & 7)) & 1)

obj_type, obj_ver = struct.unpack_from('<HH', obj, 8)
obj_generation = struct.unpack_from('<Q', obj, 16)[0]
payload_size = struct.unpack_from('<I', obj, 56)[0]
logical_size = struct.unpack_from('<Q', obj, 96)[0]
extent_count = struct.unpack_from('<I', obj, 200)[0]
page_count = struct.unpack_from('<I', obj, 224)[0]
print('DIAG_SELECTED_FILE '
      f'block={file_block} allocated={allocated(file_block)} type={obj_type} '
      f'version={obj_ver} generation={obj_generation} payload={payload_size} '
      f'logical_size={logical_size} extent_count={extent_count} '
      f'page_count={page_count} checkpoint_generation={checkpoint_generation}')

for pi in range(page_count):
    page_block = struct.unpack_from('<Q', obj, 232 + 8*pi)[0]
    if not (0 < page_block < total_blocks):
        print(f'DIAG_EXTENT_PAGE index={pi} block={page_block} OUT_OF_RANGE')
        continue
    with open(image, 'rb') as f:
        f.seek(page_block * 4096)
        page = f.read(4096)
    magic = page[:8].decode('ascii', errors='replace')
    page_generation = struct.unpack_from('<Q', page, 8)[0]
    entries = struct.unpack_from('<I', page, 32)[0]
    bytes_used = struct.unpack_from('<I', page, 36)[0]
    print('DIAG_EXTENT_PAGE '
          f'index={pi} block={page_block} allocated={allocated(page_block)} '
          f'magic={magic} generation={page_generation} entries={entries} bytes={bytes_used}')
    next_logical = 0
    normal_blocks = hole_blocks = missing = 0
    for j in range(entries):
        off = 80 + j * 24
        logical, physical, blocks, flags = struct.unpack_from('<QQII', page, off)
        if logical != next_logical:
            print(f'DIAG_EXTENT_GAP entry={j} expected={next_logical} got={logical}')
        next_logical = logical + blocks
        if flags == 0:
            normal_blocks += blocks
            for b in range(physical, physical + blocks):
                if not allocated(b):
                    missing += 1
                    if missing <= 8:
                        print(f'DIAG_DATA_UNALLOCATED entry={j} block={b}')
        elif flags == 1:
            hole_blocks += blocks
        else:
            print(f'DIAG_BAD_EXTENT_FLAG entry={j} flags={flags}')
    print('DIAG_EXTENT_SUMMARY '
          f'logical_end={next_logical} normal_blocks={normal_blocks} '
          f'hole_blocks={hole_blocks} missing_allocated_blocks={missing}')
PY

if "$scrub" "$image"; then
    echo 'PROMOTION_GENERATION_CLEAN_UNEXPECTEDLY'
    exit 0
fi
echo 'PROMOTION_GENERATION_CORRUPT' >&2
exit 1
