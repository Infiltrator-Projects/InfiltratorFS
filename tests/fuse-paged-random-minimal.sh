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

mount_image() {
    : >"$log"
    "$fuse" "$image" "$mnt" -f >"$log" 2>&1 &
    pid=$!
    for _ in $(seq 1 100); do
        mountpoint -q "$mnt" && return 0
        kill -0 "$pid" 2>/dev/null || { cat "$log" >&2; return 1; }
        sleep 0.1
    done
    return 1
}

unmount_image() {
    fusermount3 -u "$mnt"
    wait "$pid" 2>/dev/null || true
    pid=""
}

raw_promotion_diagnosis() {
    "$forensic" --jsonl "$image" >"$forensic_json"
    python3 - "$image" "$forensic_json" <<'PY'
import json, struct, sys
image, report = sys.argv[1:3]
records = []
with open(report, 'r', encoding='utf-8') as f:
    for line in f:
        r = json.loads(line)
        if r.get('record') == 'metadata':
            records.append(r)
files = [r for r in records
         if r.get('state') == 'current' and r.get('object_type') == 'file']
print(f'DIAG_CURRENT_FILES count={len(files)}')
if not files:
    raise SystemExit(0)
# The minimal reproducer has one user file. Hidden adapter metadata, if any,
# remains classic/small, so select the current version-2 file explicitly.
paged = [r for r in files if r.get('object_version') == 2]
print(f'DIAG_PAGED_FILES count={len(paged)}')
if not paged:
    raise SystemExit(0)
r = paged[0]
file_block = int(r['block'])
with open(image, 'rb') as f:
    f.seek(0)
    sb = f.read(4096)
    generation = struct.unpack_from('<Q', sb, 20)[0]
    total_blocks = struct.unpack_from('<Q', sb, 28)[0]
    bitmap_start = struct.unpack_from('<Q', sb, 44)[0]
    bitmap_blocks = struct.unpack_from('<Q', sb, 52)[0]
    f.seek(bitmap_start * 4096)
    bitmap = f.read(bitmap_blocks * 4096)
    f.seek(file_block * 4096)
    obj = f.read(4096)

def bit(block):
    return (bitmap[block >> 3] >> (block & 7)) & 1

obj_type, obj_ver = struct.unpack_from('<HH', obj, 8)
obj_generation = struct.unpack_from('<Q', obj, 16)[0]
payload_size = struct.unpack_from('<I', obj, 56)[0]
logical_size = struct.unpack_from('<Q', obj, 96)[0]
extent_count = struct.unpack_from('<I', obj, 200)[0]
page_count = struct.unpack_from('<I', obj, 224)[0]
print('DIAG_FILE '
      f'block={file_block} allocated={bit(file_block)} type={obj_type} '
      f'version={obj_ver} generation={obj_generation} payload={payload_size} '
      f'logical_size={logical_size} extent_count={extent_count} '
      f'page_count={page_count} checkpoint_generation={generation}')

page_ptrs = [struct.unpack_from('<Q', obj, 232 + 8*i)[0]
             for i in range(page_count)]
for pi, page_block in enumerate(page_ptrs):
    if not page_block or page_block >= total_blocks:
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
          f'index={pi} block={page_block} allocated={bit(page_block)} '
          f'magic={magic} generation={page_generation} '
          f'entries={entries} bytes={bytes_used}')
    normal = holes = missing = 0
    next_logical = 0
    for j in range(entries):
        off = 80 + j * 24
        logical, physical, blocks, flags = struct.unpack_from('<QQII', page, off)
        if logical != next_logical:
            print(f'DIAG_EXTENT_GAP page={pi} entry={j} expected={next_logical} got={logical}')
        next_logical = logical + blocks
        if flags == 0:
            normal += blocks
            for b in range(physical, physical + blocks):
                if b >= total_blocks or not bit(b):
                    missing += 1
                    if missing <= 8:
                        print(f'DIAG_DATA_UNALLOCATED entry={j} block={b}')
        elif flags == 1:
            holes += blocks
        else:
            print(f'DIAG_BAD_EXTENT_FLAG page={pi} entry={j} flags={flags}')
    print('DIAG_EXTENT_SUMMARY '
          f'page={pi} logical_end={next_logical} normal_blocks={normal} '
          f'hole_blocks={holes} missing_allocated_blocks={missing}')
PY
}

truncate -s 1G "$image"
"$mkfs" -L MinimalPagedPromotion "$image" >/dev/null
mkdir -p "$mnt"
mount_image

# Phase A: execute exactly writes 0..80. Write 80 is the first operation whose
# extent vector exceeds the 161 classic entries and therefore promotes the
# file to Format 0.12 paged extents. Publish that state deliberately.
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
        off = logical * 4096
        block[0:8] = i.to_bytes(8, 'little')
        n = os.pwrite(fd, block, off)
        if n != 4096:
            raise RuntimeError(f'short pwrite {n} at i={i}')
        if i in (79, 80):
            print(f'PROMOTION_PWRITE_OK i={i} logical={logical}', flush=True)
    os.fsync(fd)
    print('PROMOTION_FSYNC_OK i=80', flush=True)
finally:
    os.close(fd)
PY
unmount_image

echo 'PROMOTION_OFFLINE_CHECK'
"$inspect" "$image"
raw_promotion_diagnosis
if ! "$scrub" "$image"; then
    echo 'PROMOTION_GENERATION_CORRUPT' >&2
    exit 1
fi
echo 'PROMOTION_GENERATION_CLEAN'

# Phase B: remount the known-clean promoted generation and execute only the next
# RNG write. If this fails, unmount without a follow-up sync and prove whether
# that failed COW operation damaged the already-published generation.
mount_image
set +e
python3 - "$mnt" <<'PY'
import os, random, sys
path = os.path.join(sys.argv[1], 'random-update.bin')
rng = random.Random(0x1F11F5)
logical = None
for i in range(82):
    logical = rng.randrange(0, (512 * 1024 * 1024) // 4096)
assert logical is not None
fd = os.open(path, os.O_RDWR)
try:
    block = bytearray(4096)
    block[0:8] = (81).to_bytes(8, 'little')
    off = logical * 4096
    try:
        n = os.pwrite(fd, block, off)
    except OSError as exc:
        print(f'FOLLOWUP_PWRITE_FAIL i=81 logical={logical} offset={off} errno={exc.errno}',
              file=sys.stderr, flush=True)
        raise
    if n != 4096:
        raise RuntimeError(f'short pwrite {n} at i=81')
    print(f'FOLLOWUP_PWRITE_OK i=81 logical={logical}', flush=True)
finally:
    os.close(fd)
PY
followup_rc=$?
set -e

# Deliberately avoid fsync here. A failed transaction is not allowed to change
# any bytes reachable from the published promotion checkpoint.
unmount_image

echo 'FOLLOWUP_OFFLINE_CHECK'
"$inspect" "$image" || true
if ! "$scrub" "$image"; then
    echo 'FOLLOWUP_DAMAGED_PUBLISHED_GENERATION' >&2
    exit 1
fi
if [[ "$followup_rc" -ne 0 ]]; then
    echo "follow-up write failed rc=$followup_rc but published generation survived" >&2
    exit "$followup_rc"
fi

echo 'fuse-paged-random-minimal: PASS'
