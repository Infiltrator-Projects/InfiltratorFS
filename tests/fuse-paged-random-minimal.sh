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

set +e
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
    for i in range(100):
        logical = rng.randrange(0, size // 4096)
        off = logical * 4096
        block[0:8] = i.to_bytes(8, 'little')
        try:
            n = os.pwrite(fd, block, off)
        except OSError as exc:
            print(f'MINIMAL_PWRITE_FAIL i={i} logical={logical} offset={off} errno={exc.errno}',
                  file=sys.stderr, flush=True)
            raise
        if n != 4096:
            raise RuntimeError(f'short pwrite {n} at i={i}')
        if i in (79, 80, 81, 82):
            print(f'MINIMAL_PWRITE_OK i={i} logical={logical}', flush=True)
finally:
    os.close(fd)
PY
rc=$?
set -e

fusermount3 -u "$mnt"
wait "$pid" 2>/dev/null || true
pid=""

"$inspect" "$image" || true
if ! "$scrub" "$image"; then
    echo 'MINIMAL_COMMITTED_GENERATION_CORRUPT' >&2
    exit 1
fi
if [[ "$rc" -ne 0 ]]; then
    echo "minimal random write failed rc=$rc but committed generation survived" >&2
    exit "$rc"
fi

echo 'fuse-paged-random-minimal: PASS'
