#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail

build_dir="${1:-build}"
mkfs="$build_dir/mkfs.infilfs"
inspect="$build_dir/infilfs-inspect"
scrub="$build_dir/infilfs-scrub"
fuse="$build_dir/infilfs-fuse"

if [[ ! -r /dev/fuse || ! -w /dev/fuse ]] ||
   ! command -v fusermount3 >/dev/null 2>&1; then
    echo 'fuse-paged-random-minimal: SKIP (/dev/fuse access or fusermount3 unavailable)'
    exit 77
fi
for program in "$mkfs" "$inspect" "$scrub" "$fuse"; do
    if [[ ! -x "$program" ]]; then
        echo "fuse-paged-random-minimal: missing executable: $program" >&2
        exit 2
    fi
done

tmp="$(mktemp -d)"
image="$tmp/minimal.img"
mnt="$tmp/mnt"
log="$tmp/fuse.log"
pid=""

cleanup() {
    set +e
    if mountpoint -q "$mnt" 2>/dev/null; then
        fusermount3 -u "$mnt" 2>/dev/null ||
            fusermount3 -uz "$mnt" 2>/dev/null || true
    fi
    if [[ -n "$pid" ]]; then
        wait "$pid" 2>/dev/null || true
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT

mount_image() {
    mkdir -p "$mnt"
    : >"$log"
    "$fuse" "$image" "$mnt" -f >"$log" 2>&1 &
    pid=$!
    for _ in $(seq 1 100); do
        if mountpoint -q "$mnt"; then
            return 0
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            cat "$log" >&2
            return 1
        fi
        sleep 0.1
    done
    echo 'fuse-paged-random-minimal: filesystem did not mount' >&2
    cat "$log" >&2
    return 1
}

unmount_image() {
    fusermount3 -u "$mnt"
    wait "$pid" 2>/dev/null || true
    pid=""
}

# The installed-build failure first appeared when a 512 MiB sparse file crossed
# the classic extent ceiling under non-monotonic 4 KiB writes. Exercise that
# exact representation transition through the real FUSE adapter and native
# mkfs, then require the committed image to reopen and scrub clean.
truncate -s 1G "$image"
"$mkfs" -L MinimalPagedPromotion "$image" >/dev/null
mount_image

python3 - "$mnt" <<'PY'
import os, random, sys
root = sys.argv[1]
path = os.path.join(root, 'random-update.bin')
size = 512 * 1024 * 1024
with open(path, 'wb') as f:
    f.truncate(size)

rng = random.Random(0x1F11F5)
fd = os.open(path, os.O_RDWR)
written = {}
try:
    block = bytearray(4096)
    for i in range(320):
        logical = rng.randrange(0, size // 4096)
        block[:] = b'\0' * len(block)
        block[0:8] = i.to_bytes(8, 'little')
        n = os.pwrite(fd, block, logical * 4096)
        if n != 4096:
            raise RuntimeError(f'short pwrite {n} at i={i}')
        written[logical] = i
    os.fsync(fd)

    # Verify a deterministic sample through the mounted read path. Duplicate
    # random locations intentionally use the last value written to that block.
    for logical, expected in list(written.items())[::max(1, len(written)//16)]:
        data = os.pread(fd, 4096, logical * 4096)
        if len(data) != 4096 or int.from_bytes(data[:8], 'little') != expected:
            raise RuntimeError(
                f'readback mismatch logical={logical} expected={expected}')
finally:
    os.close(fd)
PY

unmount_image
"$inspect" "$image" >/dev/null
"$scrub" "$image" | grep -Fq 'Result:              CLEAN'

# Reopen once more through FUSE to prove the promoted file remains usable after
# an offline strict graph validation and fresh mount.
mount_image
python3 - "$mnt" <<'PY'
import os, sys
path = os.path.join(sys.argv[1], 'random-update.bin')
st = os.stat(path)
if st.st_size != 512 * 1024 * 1024:
    raise RuntimeError(f'unexpected size after remount: {st.st_size}')
fd = os.open(path, os.O_RDONLY)
try:
    if len(os.pread(fd, 4096, 0)) != 4096:
        raise RuntimeError('short read after remount')
finally:
    os.close(fd)
PY
unmount_image
"$scrub" "$image" | grep -Fq 'Result:              CLEAN'

echo 'fuse-paged-random-minimal: PASS'
