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
    echo 'hyperv-mounted-stress: SKIP (/dev/fuse access or fusermount3 unavailable)'
    exit 77
fi
for program in "$mkfs" "$inspect" "$scrub" "$fuse"; do
    if [[ ! -x "$program" ]]; then
        echo "hyperv-mounted-stress: missing executable: $program" >&2
        exit 2
    fi
done

tmp="$(mktemp -d)"
image="$tmp/hyperv-stress.img"
mount_dir="$tmp/mnt"
fuse_log="$tmp/fuse.log"
fuse_pid=""
current_step="initialization"
random_rc=0

report_error() {
    local line="$1" command="$2" status="$3"
    set +e
    echo "hyperv-mounted-stress: FAIL during $current_step" >&2
    echo "hyperv-mounted-stress: line $line exited $status: $command" >&2
    if [[ -s "$fuse_log" ]]; then
        echo 'hyperv-mounted-stress: FUSE log follows:' >&2
        tail -n 200 "$fuse_log" | sed 's/^/  /' >&2
    fi
}

cleanup() {
    set +e
    if mountpoint -q "$mount_dir" 2>/dev/null; then
        fusermount3 -u "$mount_dir" 2>/dev/null ||
            fusermount3 -uz "$mount_dir" 2>/dev/null || true
    fi
    if [[ -n "$fuse_pid" ]]; then
        wait "$fuse_pid" 2>/dev/null || true
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT
trap 'report_error "$LINENO" "$BASH_COMMAND" "$?"' ERR

step() {
    current_step="$1"
    echo
    echo "hyperv-mounted-stress: $current_step"
}

mount_image() {
    mkdir -p "$mount_dir"
    : >"$fuse_log"
    "$fuse" "$image" "$mount_dir" -f >"$fuse_log" 2>&1 &
    fuse_pid=$!
    for _ in $(seq 1 100); do
        if mountpoint -q "$mount_dir"; then
            return 0
        fi
        kill -0 "$fuse_pid" 2>/dev/null || break
        sleep 0.1
    done
    echo 'hyperv-mounted-stress: filesystem did not mount' >&2
    return 1
}

unmount_image() {
    fusermount3 -u "$mount_dir"
    wait "$fuse_pid" 2>/dev/null || true
    fuse_pid=""
}

strict_committed_check() {
    local label="$1"
    echo "hyperv-mounted-stress: strict committed check: $label"
    "$inspect" "$image"
    "$scrub" "$image"
}

# Match the real qualification image size and the high-index workload that
# preceded the failure on Linux Mint 22.3 under Hyper-V. The image is sparse on
# the host, so this does not consume 8 GiB merely by being created.
step 'formatting 8 GiB Format 0.12 image'
truncate -s 8G "$image"
"$mkfs" -L HyperV-Mounted-Stress "$image"

step 'mounting through the real FUSE adapter'
mount_image

step 'creating 5000 small files to force a multi-page object index'
python3 - "$mount_dir" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1]) / 'small-files'
root.mkdir()
payload = b'InfiltratorFS-small-file\n'
for i in range(5000):
    (root / f'f{i:06d}.dat').write_bytes(payload + str(i).encode())
PY
[[ "$(find "$mount_dir/small-files" -maxdepth 1 -type f | wc -l)" -eq 5000 ]]

step 'performing 200 explicit 4 KiB fsync publications'
python3 - "$mount_dir" <<'PY'
import os, sys
path = os.path.join(sys.argv[1], 'fsync-4k.bin')
fd = os.open(path, os.O_CREAT | os.O_TRUNC | os.O_WRONLY, 0o644)
try:
    block = b'F' * 4096
    for i in range(200):
        os.write(fd, block)
        os.fsync(fd)
finally:
    os.close(fd)
PY

step 'writing and publishing the 1 GiB sequential file'
dd if=/dev/zero of="$mount_dir/large-zero.bin" bs=4M count=256 conv=fsync status=none
sync

# This is the critical discriminator. If it fails, generation damage predates
# the random workload. If it passes and the later transaction breaks the same
# generation, operation-level COW isolation is at fault.
step 'strictly validating the committed generation before random writes'
strict_committed_check 'before-random'

step 'creating 512 MiB sparse file and replaying exact RNG overwrite sequence'
set +e
python3 - "$mount_dir" "$image" "$scrub" <<'PY'
import os, random, subprocess, sys
root, image, scrub = sys.argv[1:4]
path = os.path.join(root, 'random-update.bin')
size = 512 * 1024 * 1024
with open(path, 'wb') as f:
    f.truncate(size)
rng = random.Random(0x1F11F5)
fd = os.open(path, os.O_RDWR)
try:
    block = bytearray(4096)
    for i in range(4000):
        logical = rng.randrange(0, size // 4096)
        off = logical * 4096
        block[0:8] = i.to_bytes(8, 'little')
        try:
            n = os.pwrite(fd, block, off)
        except OSError as exc:
            print(
                f'RANDOM_PWRITE_FAIL i={i} logical={logical} offset={off} '
                f'errno={exc.errno} error={exc}', file=sys.stderr, flush=True)
            raise
        if n != 4096:
            raise RuntimeError(
                f'short pwrite i={i} logical={logical} offset={off} n={n}')
        if i and i % 256 == 0:
            print(f'RANDOM_FSYNC i={i} logical={logical}', flush=True)
            os.fsync(fd)
            # Every explicitly published random-write generation must be
            # independently openable and scrub-clean while FUSE remains up.
            subprocess.run([scrub, image], check=True)
    os.fsync(fd)
finally:
    os.close(fd)
PY
random_rc=$?
set -e

if [[ "$random_rc" -ne 0 ]]; then
    echo "hyperv-mounted-stress: random phase failed rc=$random_rc" >&2
    step 'unmounting WITHOUT another sync after random failure'
    unmount_image
    step 'inspecting last committed checkpoint after failed transaction abort'
    "$inspect" "$image" || true
    if "$scrub" "$image"; then
        echo 'hyperv-mounted-stress: committed generation survived failed random transaction' >&2
    else
        echo 'COMMITTED_GENERATION_CORRUPTED_BY_FAILED_TRANSACTION' >&2
    fi
    exit "$random_rc"
fi

step 'unmounting successful workload'
unmount_image
step 'strict offline reopen and scrub after all 4000 random writes'
"$inspect" "$image"
"$scrub" "$image"

echo 'hyperv-mounted-stress: PASS'
