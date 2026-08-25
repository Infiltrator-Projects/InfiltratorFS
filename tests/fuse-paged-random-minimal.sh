#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail

build_dir="${1:-build}"
mkfs="$build_dir/mkfs.infilfs"
scrub="$build_dir/infilfs-scrub"
inspect="$build_dir/infilfs-inspect"
fuse="$build_dir/infilfs-fuse"

tmp="$(mktemp -d)"
image="$tmp/minimal.img"
mnt="$tmp/mnt"
log="$tmp/fuse.log"
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
