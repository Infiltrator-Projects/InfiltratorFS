#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Complete InfiltratorFS release regression, performance and destructive native
# qualification for Shannon's dedicated /dev/mmcblk0p22 test partition.
#
# THIS SCRIPT DESTROYS /dev/mmcblk0p22 AND CAN RUN FOR SEVERAL HOURS.
#
set -Eeuo pipefail

TARGET="/dev/mmcblk0p22"
EXPECTED_PARENT="/dev/mmcblk0"
EXPECTED_PARTNO="22"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(sed -nE 's/^project\(InfiltratorFS VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES C\)$/\1/p' "$ROOT/CMakeLists.txt")"
TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
LOG_DIR="${HOME}/Downloads"
[[ -d "$LOG_DIR" ]] || LOG_DIR="$PWD"
LOG="$LOG_DIR/infiltratorfs-complete-qualification-$VERSION-$TIMESTAMP.log"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/infiltratorfs-complete.XXXXXX")"
BUILD="$WORK/build"
DIST="$WORK/dist"
START_TIME="$(date '+%Y-%m-%d %H:%M:%S')"
START_EPOCH="$(date +%s)"
MODULE_LOADED=0

MIN_SEQ_WRITE_MIB_S="${INFS_MIN_SEQ_WRITE_MIB_S:-15}"
MIN_SEQ_READ_MIB_S="${INFS_MIN_SEQ_READ_MIB_S:-75}"
MIN_RANDOM_IOPS="${INFS_MIN_RANDOM_IOPS:-65}"
MAX_FSYNC_P95_MS="${INFS_MAX_FSYNC_P95_MS:-250}"
MIN_TEMP_FREE_GIB="${INFS_MIN_TEMP_FREE_GIB:-20}"

cleanup() {
    local rc=$?
    trap - EXIT INT TERM
    set +e
    if (( MODULE_LOADED )); then
        sudo rmmod infiltratorfs >/dev/null 2>&1 || true
    fi
    rm -rf -- "$WORK"
    if (( rc != 0 )); then
        printf '\n[FAIL] Complete qualification aborted with exit code %d.\n' "$rc"
        printf '[FAIL] Log retained at %s\n' "$LOG"
    fi
    exit "$rc"
}
trap cleanup EXIT INT TERM

fatal() {
    printf '[FATAL] %s\n' "$*" >&2
    exit 2
}

section() {
    printf '\n================================================================\n%s\n================================================================\n' "$1"
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || fatal "Required command is missing: $1"
}

run_timed() {
    local label="$1" limit="$2"
    shift 2
    local before after elapsed
    before="$(date +%s)"
    timeout --foreground --signal=TERM --kill-after=60s "$limit" "$@"
    after="$(date +%s)"
    elapsed=$((after - before))
    printf '[TIME] %s: %d s\n' "$label" "$elapsed"
}

require_number() {
    [[ "$2" =~ ^[0-9]+([.][0-9]+)?$ ]] || fatal "Could not parse $1 performance value: $2"
}

require_ge() {
    local label="$1" value="$2" floor="$3"
    require_number "$label" "$value"
    awk -v value="$value" -v floor="$floor" 'BEGIN { exit !(value >= floor) }' ||
        fatal "$label regression: $value is below required floor $floor"
    printf '[PASS] %s: %s (floor %s)\n' "$label" "$value" "$floor"
}

require_le() {
    local label="$1" value="$2" ceiling="$3"
    require_number "$label" "$value"
    awk -v value="$value" -v ceiling="$ceiling" 'BEGIN { exit !(value <= ceiling) }' ||
        fatal "$label regression: $value exceeds allowed ceiling $ceiling"
    printf '[PASS] %s: %s (ceiling %s)\n' "$label" "$value" "$ceiling"
}

(( EUID != 0 )) || fatal "Run this script as the ordinary desktop user; it invokes sudo only where required."
[[ -n "$VERSION" ]] || fatal "Could not determine the source version."
cd "$ROOT"
touch "$LOG"
exec > >(tee -a "$LOG") 2>&1

section "Complete InfiltratorFS $VERSION qualification preflight"
printf 'Source:      %s\nTarget:      %s\nKernel:      %s\nTemporary:   %s\nLog:         %s\nStarted:     %s\n'     "$ROOT" "$TARGET" "$(uname -r)" "$WORK" "$LOG" "$(date --iso-8601=seconds)"

for cmd in awk bash blockdev cmake cp ctest cut date df dpkg dpkg-deb dpkg-query \
    find findmnt git grep head insmod lsblk make mkdir mktemp modinfo modprobe \
    python3 readlink rm rmmod sed sha256sum sort sudo tail tee timeout touch tr \
    uname; do
    require_cmd "$cmd"
done

[[ -b "$TARGET" ]] || fatal "$TARGET is not a block device."
[[ "$(readlink -f "$TARGET")" == "$TARGET" ]] || fatal "Target resolves away from $TARGET."
parent_name="$(lsblk -ndo PKNAME "$TARGET" | head -n1 | tr -d '[:space:]')"
partno="$(lsblk -ndo PARTN "$TARGET" 2>/dev/null | head -n1 | tr -d '[:space:]' || true)"
[[ "/dev/$parent_name" == "$EXPECTED_PARENT" ]] || fatal "Target parent is /dev/$parent_name, expected $EXPECTED_PARENT."
[[ -z "$partno" || "$partno" == "$EXPECTED_PARTNO" ]] || fatal "Target partition number is $partno, expected $EXPECTED_PARTNO."
[[ "$TARGET" == "${EXPECTED_PARENT}p${EXPECTED_PARTNO}" ]] || fatal "Partition-22 safety gate mismatch."
[[ "$(findmnt -rn -o SOURCE / | head -n1)" != "$TARGET" ]] || fatal "Refusing to destroy the root filesystem."

sudo -v
load_plan="$(modprobe --dry-run --verbose infiltratorfs 2>/dev/null || true)"
case "$load_plan" in
    *"install /bin/false"*|*"install /usr/bin/false"*|*"install false"*)
        fatal "InfiltratorFS module loading is administratively disabled. Remove the emergency modprobe override before this qualification."
        ;;
esac

if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    [[ -z "$(git status --porcelain --untracked-files=no)" ]] || fatal "Tracked source files are modified."
    exact_tag="$(git describe --tags --exact-match HEAD 2>/dev/null || true)"
    [[ "$exact_tag" == "v$VERSION" ]] || fatal "Checkout must be the exact release tag v$VERSION; found ${exact_tag:-untagged}."
fi

installed_version="$(dpkg-query -W -f='${Version}' infiltratorfs 2>/dev/null || true)"
case "$installed_version" in
    "$VERSION"|"$VERSION+native"*) ;;
    *) fatal "Installed package is '${installed_version:-missing}', expected $VERSION or $VERSION+nativeN." ;;
esac
printf '[PASS] Installed release identity: infiltratorfs %s\n' "$installed_version"

temp_free_kib="$(df -Pk "$WORK" | awk 'NR==2 {print $4}')"
required_kib=$((MIN_TEMP_FREE_GIB * 1024 * 1024))
(( temp_free_kib >= required_kib )) ||
    fatal "Temporary filesystem needs at least $MIN_TEMP_FREE_GIB GiB free; only $((temp_free_kib / 1024 / 1024)) GiB is available."
printf '[PASS] Temporary free space: %d GiB\n' "$((temp_free_kib / 1024 / 1024))"

printf '\nWARNING: THIS ERASES ALL DATA ON %s AND RUNS THE COMPLETE MULTI-HOUR SUITE.\n' "$TARGET"
printf 'Type exactly: ERASE /dev/mmcblk0p22 AND RUN COMPLETE QUALIFICATION\n> '
read -r confirmation
[[ "$confirmation" == "ERASE /dev/mmcblk0p22 AND RUN COMPLETE QUALIFICATION" ]] || fatal "Cancelled."

section "Strict userspace build and complete portable suite"
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
cmake --build "$BUILD" --parallel
ctest --test-dir "$BUILD" --output-on-failure
test ! -e "$BUILD/infilfs-fuse"

section "All source, format, native and workload policy guards"
bash tests/native-random-write-optimization-policy.sh .
bash tests/native-sequential-write-scaling-policy.sh .
bash tests/native-small-file-scaling-policy.sh .
bash tests/native-free-extent-index-policy.sh .
bash tests/native-parallel-allocation-policy.sh .
bash tests/native-workload-placement-policy.sh .
bash tests/native-media-placement-policy.sh .
bash tests/native-defrag-policy.sh .
bash tests/native-index-tree-policy.sh .
bash tests/index-tree-format-policy.sh .
bash tests/allocation-tree-format-policy.sh .
bash tests/directory-tree-policy.sh .
bash tests/native-scale-policy.sh .
bash tests/native-endurance-policy.sh .
bash -n tests/native-partition22-full-test.sh
bash -n tests/native-partition22-qualification.sh
bash -n tests/native-defrag-qualification.sh
bash -n tests/native-media-placement-qualification.sh
bash -n tests/native-scale-qualification.sh
bash -n tests/native-endurance-qualification.sh
PYTHONPYCACHEPREFIX="$WORK/pycache" python3 -m py_compile     tests/native-scale-stress.py tests/native-endurance-stress.py

section "Exact running-kernel module and DKMS-source reproduction"
KDIR="/lib/modules/$(uname -r)/build"
[[ -f "$KDIR/Makefile" ]] || fatal "Matching kernel headers are missing at $KDIR."
make -C kernel clean KDIR="$KDIR"
make -C kernel KDIR="$KDIR"
test "$(modinfo -F vermagic kernel/infiltratorfs.ko | awk '{print $1}')" = "$(uname -r)"
dkms_copy="$WORK/dkms-source"
mkdir -p "$dkms_copy"
for file in Makefile infiltratorfs.c infiltratorfs_format.h     infiltratorfs_allocation_map.inc infiltratorfs_allocation_publish.inc     infiltratorfs_parallel_alloc.inc     infiltratorfs_index_tree.inc infiltratorfs_directory_tree.inc     infiltratorfs_rw.inc infiltratorfs_rw_legacy.inc infiltratorfs_rw_data.inc     infiltratorfs_rw_namespace.inc infiltratorfs_rw_read_cache.inc     infiltratorfs_pagecache.inc infiltratorfs_linux_meta.inc     infiltratorfs_defrag.inc infiltratorfs_ioctl.h; do
    cp "kernel/$file" "$dkms_copy/$file"
done
make -C "$dkms_copy" KDIR="$KDIR"
test "$(modinfo -F vermagic "$dkms_copy/infiltratorfs.ko" | awk '{print $1}')" = "$(uname -r)"

section "Native Debian and self-extracting installer packages"
bash packaging/build-linux-packages.sh "$BUILD" "$DIST"
installer="$DIST/infiltratorfs-$VERSION-linux-native.run"
deb="$DIST/infiltratorfs_${VERSION}_$(dpkg --print-architecture).deb"
[[ -x "$installer" && -s "$deb" ]] || fatal "Expected native release packages were not built."
"$installer" --verify
(
    cd "$DIST"
    sha256sum -c "$(basename "$deb").sha256"
    sha256sum -c "$(basename "$installer").sha256"
)
deb_contents_file="$WORK/deb-contents.txt"
dpkg-deb --contents "$deb" > "$deb_contents_file"
grep -q 'usr/bin/infilfs-optimize$' "$deb_contents_file"
! grep -q 'usr/bin/infilfs-fuse$' "$deb_contents_file"

section "Installed-release destructive partition-22 qualification"
partition_started="$(date +%s)"
run_timed "partition-22 full native qualification" 3h     bash tests/native-partition22-full-test.sh --destroy-partition-22
partition_log="$(find "$ROOT" -maxdepth 1 -type f     -name 'infiltratorfs-partition22-test-*.log'     -newermt "@$partition_started" -printf '%T@ %p\n' |
    sort -nr | head -n1 | cut -d' ' -f2-)"
[[ -n "$partition_log" && -f "$partition_log" ]] || fatal "Could not locate the partition-22 qualification log."
printf '[PASS] Partition qualification log: %s\n' "$partition_log"

seq_write="$(sed -nE 's/.*sequential write: ([0-9.]+) MiB\/s.*/\1/p' "$partition_log" | tail -n1)"
seq_read="$(sed -nE 's/.*verified sequential read: ([0-9.]+) MiB\/s.*/\1/p' "$partition_log" | tail -n1)"
random_iops="$(sed -nE 's/.*random 4 KiB overwrite: ([0-9.]+) IOPS.*/\1/p' "$partition_log" | tail -n1)"
fsync_p95="$(sed -nE 's/.*fsync latency:.*p95=([0-9.]+)ms.*/\1/p' "$partition_log" | tail -n1)"
require_ge "Sequential write MiB/s" "$seq_write" "$MIN_SEQ_WRITE_MIB_S"
require_ge "Verified sequential read MiB/s" "$seq_read" "$MIN_SEQ_READ_MIB_S"
require_ge "Random 4 KiB overwrite IOPS" "$random_iops" "$MIN_RANDOM_IOPS"
require_le "fsync p95 milliseconds" "$fsync_p95" "$MAX_FSYNC_P95_MS"

section "Exact-source online fragmentation and defragmentation"
run_timed "native online-defrag qualification" 30m     bash tests/native-defrag-qualification.sh "$BUILD" "$ROOT/kernel/infiltratorfs.ko"

section "Exact-source million-file and 1 TiB mounted scale"
sudo modprobe lz4_compress 2>/dev/null || true
sudo modprobe lz4_decompress 2>/dev/null || true
if ! sudo insmod "$ROOT/kernel/infiltratorfs.ko"; then
    sudo dmesg | tail -n 80 >&2 || true
    exit 1
fi
MODULE_LOADED=1
grep -qw infiltratorfs /proc/filesystems
run_timed "million-file and 1 TiB scale qualification" 4h     bash tests/native-scale-qualification.sh "$BUILD"

section "Exact-source near-full and five-minute mixed-workload endurance"
run_timed "near-full mixed-workload endurance qualification" 2h     bash tests/native-endurance-qualification.sh "$BUILD"
sudo rmmod infiltratorfs
MODULE_LOADED=0

section "Final physical-media scrub and kernel diagnostics"
sudo "$BUILD/infilfs-scrub" "$TARGET" | tee "$WORK/final-partition22-scrub.txt"
grep -Fq 'Result:              CLEAN' "$WORK/final-partition22-scrub.txt"
kernel_failures="$(sudo dmesg --since "$START_TIME" 2>/dev/null |
    grep -E 'EUCLEAN|Structure needs cleaning|BUG:|Oops:|Kernel panic|hung task|soft lockup|hard LOCKUP|general protection fault' || true)"
[[ -z "$kernel_failures" ]] || {
    printf '%s\n' "$kernel_failures" >&2
    fatal "Kernel corruption, crash or lockup signature observed."
}

elapsed=$(( $(date +%s) - START_EPOCH ))
section "COMPLETE QUALIFICATION PASS"
printf '[PASS] Installed release, source, packages, portable core, native kernel,\n'
printf '       physical VFS, recovery, integrity, performance, scale, endurance,\n'
printf '       online defrag, remount and scrub qualification all passed.\n'
printf '[PASS] Version: %s\n[PASS] Elapsed: %d seconds\n[PASS] Log: %s\n'     "$VERSION" "$elapsed" "$LOG"
