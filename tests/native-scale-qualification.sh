#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Native mounted scale qualification. The exact test module must already be
# loaded and the infiltratorfs filesystem type registered before invocation.
#
set -Eeuo pipefail

if (( $# != 1 )); then
    echo "Usage: $0 <userspace-build-dir>" >&2
    exit 2
fi

BUILD="$(readlink -f "$1")"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STRESS_PY="$ROOT/tests/native-scale-stress.py"
FILE_COUNT="${INFS_SCALE_FILE_COUNT:-1000000}"
DIRECTORY_COUNT="${INFS_SCALE_DIRECTORY_COUNT:-1000}"
WORKERS="${INFS_SCALE_WORKERS:-8}"
CHURN_FILES="${INFS_SCALE_CHURN_FILES:-100000}"
FILE_IMAGE_SIZE="${INFS_SCALE_FILE_IMAGE_SIZE:-16G}"
LARGE_IMAGE_SIZE="${INFS_SCALE_LARGE_IMAGE_SIZE:-1T}"
LARGE_PROBE_BYTES="${INFS_SCALE_LARGE_PROBE_BYTES:-268435456}"
LARGE_SPARSE_BYTES="${INFS_SCALE_LARGE_SPARSE_BYTES:-966367641600}"

for path in "$BUILD/mkfs.infilfs" "$BUILD/infilfs-scrub" "$BUILD/infilfs-inspect" "$STRESS_PY"; do
    [[ -e "$path" ]] || { echo "Missing qualification input: $path" >&2; exit 1; }
done
for cmd in python3 sudo losetup mount umount truncate findmnt timeout df du awk grep sync tee; do
    command -v "$cmd" >/dev/null 2>&1 || { echo "Missing command: $cmd" >&2; exit 1; }
done
grep -qw infiltratorfs /proc/filesystems || {
    echo "The exact native InfiltratorFS module is not registered." >&2
    exit 1
}

TMP_BASE="${RUNNER_TEMP:-/tmp}"
WORK="$(mktemp -d "$TMP_BASE/infiltratorfs-scale.XXXXXX")"
FILE_IMAGE="$WORK/million-files.img"
FILE_MOUNT="$WORK/million-files-mnt"
LARGE_IMAGE="$WORK/large-volume.img"
LARGE_MOUNT="$WORK/large-volume-mnt"
FILE_LOOP=""
LARGE_LOOP=""
FILE_MOUNTED=0
LARGE_MOUNTED=0
START_EPOCH="$(date +%s)"
START_TIME="$(date '+%Y-%m-%d %H:%M:%S')"

cleanup() {
    rc=$?
    set +e
    if (( LARGE_MOUNTED )); then timeout 20s sudo umount -l "$LARGE_MOUNT" || true; fi
    if [[ -n "$LARGE_LOOP" ]]; then timeout 20s sudo losetup -d "$LARGE_LOOP" || true; fi
    if (( FILE_MOUNTED )); then timeout 20s sudo umount -l "$FILE_MOUNT" || true; fi
    if [[ -n "$FILE_LOOP" ]]; then timeout 20s sudo losetup -d "$FILE_LOOP" || true; fi
    rm -rf "$WORK"
    exit "$rc"
}
trap cleanup EXIT INT TERM

perf() { printf '[SCALE-PERF] %s\n' "$*"; }

timed() {
    local label="$1"
    shift
    local start end rc elapsed
    start="$(date +%s%N)"
    set +e
    "$@"
    rc=$?
    set -e
    end="$(date +%s%N)"
    elapsed="$(awk -v a="$start" -v b="$end" 'BEGIN {printf "%.3f", (b-a)/1000000000}')"
    perf "$label elapsed=${elapsed}s rc=$rc"
    return "$rc"
}

kernel_failures() {
    sudo dmesg --since "$START_TIME" 2>/dev/null | \
        grep -E 'EUCLEAN|Structure needs cleaning|BUG:|Oops:|Kernel panic|hung task|soft lockup|hard LOCKUP|general protection fault' || true
}

echo "=== Million-file mounted stress ==="
mkdir -p "$FILE_MOUNT"
truncate -s "$FILE_IMAGE_SIZE" "$FILE_IMAGE"
timed "million-volume-format" sudo env SUDO_UID="$(id -u)" SUDO_GID="$(id -g)" \
    "$BUILD/mkfs.infilfs" -L MillionFileScale "$FILE_IMAGE"
# The hosted qualification uses a sparse regular file as a synthetic block
# device. Direct I/O prevents every filesystem block from being cached a
# second time by the loop backing file, so the million-file test measures the
# filesystem/VFS working set rather than an artificial double page cache.
FILE_LOOP="$(sudo losetup --direct-io=on --find --show "$FILE_IMAGE")"
timed "million-volume-mount-rw" sudo mount -t infiltratorfs -o rw "$FILE_LOOP" "$FILE_MOUNT"
FILE_MOUNTED=1
test "$(findmnt -rn -T "$FILE_MOUNT" -o FSTYPE)" = infiltratorfs

timed "million-file-workload" python3 "$STRESS_PY" "$FILE_MOUNT/million-files" \
    --files "$FILE_COUNT" --directories "$DIRECTORY_COUNT" \
    --workers "$WORKERS" --churn-files "$CHURN_FILES"

sync
printf 'Million-file image allocation after workload:\n'
du -h "$FILE_IMAGE" || true
df -hT "$FILE_MOUNT"
timed "million-volume-unmount" sudo umount "$FILE_MOUNT"
FILE_MOUNTED=0

million_scrub="$WORK/million-scrub.txt"
start="$(date +%s%N)"
set +e
timeout --signal=TERM --kill-after=30s 3600s "$BUILD/infilfs-scrub" "$FILE_IMAGE" | tee "$million_scrub"
rc=${PIPESTATUS[0]}
set -e
end="$(date +%s%N)"
elapsed="$(awk -v a="$start" -v b="$end" 'BEGIN {printf "%.3f", (b-a)/1000000000}')"
perf "million-volume-offline-scrub elapsed=${elapsed}s rc=$rc"
(( rc == 0 )) || exit "$rc"
grep -Fq 'Result:              CLEAN' "$million_scrub"

timed "million-volume-mount-ro" sudo mount -t infiltratorfs -o ro "$FILE_LOOP" "$FILE_MOUNT"
FILE_MOUNTED=1
timed "million-file-remount-verify" python3 "$STRESS_PY" "$FILE_MOUNT/million-files" \
    --files "$FILE_COUNT" --directories "$DIRECTORY_COUNT" \
    --workers "$WORKERS" --churn-files "$CHURN_FILES" --verify-only
timed "million-volume-final-unmount" sudo umount "$FILE_MOUNT"
FILE_MOUNTED=0
sudo losetup -d "$FILE_LOOP"
FILE_LOOP=""
rm -f "$FILE_IMAGE"

echo "=== 1 TiB mounted large-volume stress ==="
mkdir -p "$LARGE_MOUNT"
truncate -s "$LARGE_IMAGE_SIZE" "$LARGE_IMAGE"
timed "large-volume-format" sudo env SUDO_UID="$(id -u)" SUDO_GID="$(id -g)" \
    "$BUILD/mkfs.infilfs" -L LargeVolumeScale "$LARGE_IMAGE"
"$BUILD/infilfs-inspect" "$LARGE_IMAGE"
LARGE_LOOP="$(sudo losetup --direct-io=on --find --show "$LARGE_IMAGE")"
timed "large-volume-mount-rw" sudo mount -t infiltratorfs -o rw "$LARGE_LOOP" "$LARGE_MOUNT"
LARGE_MOUNTED=1
test "$(findmnt -rn -T "$LARGE_MOUNT" -o FSTYPE)" = infiltratorfs

python3 - "$LARGE_MOUNT" "$LARGE_PROBE_BYTES" "$LARGE_SPARSE_BYTES" <<'PY'
import hashlib
import os
import sys
import time

root = sys.argv[1]
probe_bytes = int(sys.argv[2])
sparse_bytes = int(sys.argv[3])
statvfs = os.statvfs(root)
total_bytes = statvfs.f_blocks * statvfs.f_frsize
assert total_bytes == 1 << 40, total_bytes
assert statvfs.f_namemax == 1023

work = os.path.join(root, 'large-volume')
os.mkdir(work)
pattern = bytes((index * 29 + 7) & 0xff for index in range(1024 * 1024))
path = os.path.join(work, 'probe.bin')
digest = hashlib.sha256()
started = time.monotonic()
with open(path, 'xb', buffering=0) as stream:
    left = probe_bytes
    while left:
        chunk = pattern if left >= len(pattern) else pattern[:left]
        stream.write(chunk)
        digest.update(chunk)
        left -= len(chunk)
    os.fsync(stream.fileno())
elapsed = time.monotonic() - started
print(f'[SCALE-PERF] large-volume sequential write bytes={probe_bytes} '
      f'elapsed={elapsed:.3f}s rate={probe_bytes / 1048576 / elapsed:.2f} MiB/s')

sparse = os.path.join(work, 'sparse-high-offset.bin')
fd = os.open(sparse, os.O_CREAT | os.O_RDWR | os.O_EXCL, 0o600)
try:
    os.ftruncate(fd, sparse_bytes + 4096)
    tail = b'INFILTRATORFS-LARGE-VOLUME-TAIL\n'
    assert os.pwrite(fd, tail, sparse_bytes) == len(tail)
    os.fsync(fd)
finally:
    os.close(fd)

for d in range(64):
    directory = os.path.join(work, f'd{d:02d}')
    os.mkdir(directory)
    for f in range(64):
        fd = os.open(os.path.join(directory, f'f{f:02d}'),
                     os.O_CREAT | os.O_WRONLY | os.O_EXCL, 0o600)
        os.close(fd)
os.sync()

with open(os.path.join(work, 'expected.sha256'), 'w', encoding='ascii') as stream:
    stream.write(digest.hexdigest() + '\n')
    stream.flush()
    os.fsync(stream.fileno())
print(f'[SCALE-PERF] large-volume total_bytes={total_bytes} sparse_size={sparse_bytes + 4096}')
PY

sync
df -hT "$LARGE_MOUNT"
timed "large-volume-unmount" sudo umount "$LARGE_MOUNT"
LARGE_MOUNTED=0

large_scrub="$WORK/large-scrub.txt"
start="$(date +%s%N)"
set +e
timeout --signal=TERM --kill-after=30s 1800s "$BUILD/infilfs-scrub" "$LARGE_IMAGE" | tee "$large_scrub"
rc=${PIPESTATUS[0]}
set -e
end="$(date +%s%N)"
elapsed="$(awk -v a="$start" -v b="$end" 'BEGIN {printf "%.3f", (b-a)/1000000000}')"
perf "large-volume-offline-scrub elapsed=${elapsed}s rc=$rc"
(( rc == 0 )) || exit "$rc"
grep -Fq 'Result:              CLEAN' "$large_scrub"

timed "large-volume-mount-ro" sudo mount -t infiltratorfs -o ro "$LARGE_LOOP" "$LARGE_MOUNT"
LARGE_MOUNTED=1
python3 - "$LARGE_MOUNT" "$LARGE_PROBE_BYTES" "$LARGE_SPARSE_BYTES" <<'PY'
import hashlib
import os
import sys

root = sys.argv[1]
probe_bytes = int(sys.argv[2])
sparse_bytes = int(sys.argv[3])
statvfs = os.statvfs(root)
assert statvfs.f_blocks * statvfs.f_frsize == 1 << 40
work = os.path.join(root, 'large-volume')
with open(os.path.join(work, 'expected.sha256'), encoding='ascii') as stream:
    expected = stream.read().strip()
digest = hashlib.sha256()
with open(os.path.join(work, 'probe.bin'), 'rb', buffering=0) as stream:
    while True:
        chunk = stream.read(1024 * 1024)
        if not chunk:
            break
        digest.update(chunk)
assert digest.hexdigest() == expected
sparse = os.path.join(work, 'sparse-high-offset.bin')
assert os.stat(sparse).st_size == sparse_bytes + 4096
with open(sparse, 'rb', buffering=0) as stream:
    stream.seek(sparse_bytes)
    tail = b'INFILTRATORFS-LARGE-VOLUME-TAIL\n'
    assert stream.read(len(tail)) == tail
count = 0
for d in range(64):
    with os.scandir(os.path.join(work, f'd{d:02d}')) as entries:
        count += sum(1 for entry in entries if entry.is_file(follow_symlinks=False))
assert count == 4096, count
print('Large-volume durable read-only verification: PASS')
PY

timed "large-volume-final-unmount" sudo umount "$LARGE_MOUNT"
LARGE_MOUNTED=0
sudo losetup -d "$LARGE_LOOP"
LARGE_LOOP=""

failures="$(kernel_failures)"
if [[ -n "$failures" ]]; then
    echo "$failures" >&2
    echo "Kernel corruption/lockup signature observed during scale qualification." >&2
    exit 1
fi

elapsed=$(( $(date +%s) - START_EPOCH ))
echo "Native million-file and 1 TiB mounted scale qualification: PASS (${elapsed}s)"