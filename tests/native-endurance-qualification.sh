#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Native mounted endurance qualification:
#   * drives a 4 GiB Format 0.17 image into a bounded near-full state;
#   * manufactures multiple physical free-run size classes;
#   * runs a five-minute concurrent mixed metadata/data workload;
#   * verifies durable content after an offline scrub and read-only remount.
#
set -Eeuo pipefail

if (( $# != 1 )); then
    echo "Usage: $0 <userspace-build-dir>" >&2
    exit 2
fi

BUILD="$(readlink -f "$1")"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STRESS="$ROOT/tests/native-endurance-stress.py"
IMAGE_SIZE="${INFS_ENDURANCE_IMAGE_SIZE:-4G}"
SECONDS="${INFS_ENDURANCE_SECONDS:-300}"
WORKERS="${INFS_ENDURANCE_WORKERS:-4}"
RESERVE_MIB="${INFS_ENDURANCE_RESERVE_MIB:-384}"

for path in "$BUILD/mkfs.infilfs" "$BUILD/infilfs-scrub" "$BUILD/infilfs-inspect" "$STRESS"; do
    [[ -e "$path" ]] || { echo "Missing qualification input: $path" >&2; exit 1; }
done

for cmd in python3 sudo losetup mount umount truncate findmnt timeout df du awk grep sync tee fallocate stat; do
    command -v "$cmd" >/dev/null 2>&1 || {
        echo "Missing command: $cmd" >&2
        exit 1
    }
done

grep -qw infiltratorfs /proc/filesystems || {
    echo "The exact native InfiltratorFS module is not registered." >&2
    exit 1
}

TMP_BASE="${RUNNER_TEMP:-/tmp}"
WORK="$(mktemp -d "$TMP_BASE/infiltratorfs-endurance.XXXXXX")"
IMAGE="$WORK/endurance.img"
MOUNTPOINT="$WORK/mnt"
MANIFEST="$WORK/endurance-manifest.json"
SCRUB1="$WORK/endurance-scrub.txt"
SCRUB2="$WORK/endurance-final-scrub.txt"
LOOPDEV=""
MOUNTED=0
START_EPOCH="$(date +%s)"
START_TIME="$(date '+%Y-%m-%d %H:%M:%S')"

cleanup() {
    rc=$?
    set +e
    if (( MOUNTED )); then
        timeout 20s sudo umount -l "$MOUNTPOINT" || true
    fi
    if [[ -n "$LOOPDEV" ]]; then
        timeout 20s sudo losetup -d "$LOOPDEV" || true
    fi
    rm -rf "$WORK"
    exit "$rc"
}
trap cleanup EXIT INT TERM

perf() { printf '[ENDURANCE-PERF] %s\n' "$*"; }

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
    sudo dmesg --since "$START_TIME" 2>/dev/null |         grep -E 'EUCLEAN|Structure needs cleaning|BUG:|Oops:|Kernel panic|hung task|soft lockup|hard LOCKUP|general protection fault' || true
}

mkdir -p "$MOUNTPOINT"
truncate -s "$IMAGE_SIZE" "$IMAGE"

echo "=== Format and mount endurance image ==="
timed "format" sudo env SUDO_UID="$(id -u)" SUDO_GID="$(id -g)"     "$BUILD/mkfs.infilfs" -L NativeEndurance "$IMAGE"
"$BUILD/infilfs-inspect" "$IMAGE"

LOOPDEV="$(sudo losetup --direct-io=on --find --show "$IMAGE")"
timed "mount-rw" sudo mount -t infiltratorfs -o rw "$LOOPDEV" "$MOUNTPOINT"
MOUNTED=1
test "$(findmnt -rn -T "$MOUNTPOINT" -o FSTYPE)" = infiltratorfs
findmnt -rn -T "$MOUNTPOINT" -o OPTIONS | grep -Eq '(^|,)rw(,|$)'

echo "=== Near-full fragmentation and mixed workload ==="
timed "mixed-workload" python3 "$STRESS" "$MOUNTPOINT/endurance"     --seconds "$SECONDS" --workers "$WORKERS" --reserve-mib "$RESERVE_MIB"     --manifest-out "$MANIFEST"

python3 - "$MOUNTPOINT" <<'PY'
import os
import sys

root = sys.argv[1]
st = os.statvfs(root)
total = st.f_blocks * st.f_frsize
free = st.f_bavail * st.f_frsize
pct = 100.0 * free / total
print(f"[ENDURANCE-PERF] near-full check total={total} free={free} free_pct={pct:.2f}%")
assert free >= 64 * 1024 * 1024, free
assert pct <= 15.0, pct
PY

echo "=== Extra hole-punch/refill fragmentation cycle ==="
PUNCH="$MOUNTPOINT/endurance/punch-cycle.bin"
fallocate -l 64M "$PUNCH"
before_blocks="$(stat -c '%b' "$PUNCH")"
for offset_mib in 0 8 16 24 32 40 48 56; do
    fallocate -p -o "${offset_mib}M" -l 4M "$PUNCH"
done
after_blocks="$(stat -c '%b' "$PUNCH")"
(( after_blocks < before_blocks ))
mkdir "$MOUNTPOINT/endurance/punch-refill"
for i in $(seq -w 0 15); do
    dd if=/dev/zero of="$MOUNTPOINT/endurance/punch-refill/refill-$i.bin"         bs=1M count=2 status=none
done
sync
perf "hole-punch blocks_before=$before_blocks blocks_after=$after_blocks refill_files=16"

df -hT "$MOUNTPOINT"
du -h "$IMAGE" || true

echo "=== Durability boundary and first offline scrub ==="
timed "sync" sync
timed "unmount" sudo umount "$MOUNTPOINT"
MOUNTED=0

start="$(date +%s%N)"
set +e
timeout --signal=TERM --kill-after=30s 1800s "$BUILD/infilfs-scrub" "$IMAGE" | tee "$SCRUB1"
rc=${PIPESTATUS[0]}
set -e
end="$(date +%s%N)"
elapsed="$(awk -v a="$start" -v b="$end" 'BEGIN {printf "%.3f", (b-a)/1000000000}')"
perf "offline-scrub elapsed=${elapsed}s rc=$rc"
(( rc == 0 ))
grep -Fq 'Result:              CLEAN' "$SCRUB1"

echo "=== Read-only durable verification ==="
timed "mount-ro" sudo mount -t infiltratorfs -o ro "$LOOPDEV" "$MOUNTPOINT"
MOUNTED=1
findmnt -rn -T "$MOUNTPOINT" -o OPTIONS | grep -Eq '(^|,)ro(,|$)'

timed "durable-verify" python3 "$STRESS" "$MOUNTPOINT/endurance"     --verify-manifest "$MANIFEST"

test "$(stat -c '%b' "$MOUNTPOINT/endurance/punch-cycle.bin")" -lt "$before_blocks"
test "$(find "$MOUNTPOINT/endurance/punch-refill" -maxdepth 1 -type f | wc -l)" -eq 16

timed "final-unmount" sudo umount "$MOUNTPOINT"
MOUNTED=0

echo "=== Final offline scrub ==="
start="$(date +%s%N)"
set +e
timeout --signal=TERM --kill-after=30s 1800s "$BUILD/infilfs-scrub" "$IMAGE" | tee "$SCRUB2"
rc=${PIPESTATUS[0]}
set -e
end="$(date +%s%N)"
elapsed="$(awk -v a="$start" -v b="$end" 'BEGIN {printf "%.3f", (b-a)/1000000000}')"
perf "final-offline-scrub elapsed=${elapsed}s rc=$rc"
(( rc == 0 ))
grep -Fq 'Result:              CLEAN' "$SCRUB2"

failures="$(kernel_failures)"
if [[ -n "$failures" ]]; then
    echo "$failures" >&2
    echo "Kernel corruption/lockup signature observed during endurance qualification." >&2
    exit 1
fi

elapsed=$(( $(date +%s) - START_EPOCH ))
echo "Native near-full, fragmentation and long-running mixed workload qualification: PASS (${elapsed}s)"
