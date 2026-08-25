#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# InfiltratorFS native VFS destructive integration/torture test.
#
# THIS SCRIPT DESTROYS /dev/mmcblk0p22.
#
# It is intentionally hard-wired to Shannon's dedicated partition 22 test
# target. It refuses any other block device, confirms the native kernel VFS
# rather than FUSE, formats the partition, exercises mounted I/O and metadata,
# checks integrity after unmount/remount, and records capability gaps separately
# from regressions.
#
# Run:
#   chmod +x tests/native-partition22-full-test.sh
#   tests/native-partition22-full-test.sh
#
# Non-interactive destructive confirmation:
#   tests/native-partition22-full-test.sh --destroy-partition-22
#
set -Eeuo pipefail
shopt -s nullglob

TARGET="/dev/mmcblk0p22"
EXPECTED_PARENT="/dev/mmcblk0"
EXPECTED_PARTNO="22"
MOUNTPOINT="/mnt/infiltratorfs-partition22-test"
LABEL="InfiltratorFS-Test"
ORIGINAL_PWD="${SUDO_PWD:-$PWD}"
TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
LOG="${ORIGINAL_PWD}/infiltratorfs-partition22-test-${TIMESTAMP}.log"
WORKDIR="/tmp/infiltratorfs-partition22-test-${TIMESTAMP}-$$"
CONFIRM_ARG="${1:-}"

PASS=0
FAIL=0
KNOWN=0
WARN=0
MOUNTED=0
START_EPOCH="$(date +%s)"

cleanup() {
    local rc=$?
    set +e
    if (( MOUNTED )); then
        sync
        umount "$MOUNTPOINT" >/dev/null 2>&1 || true
        MOUNTED=0
    fi
    rm -rf "$MOUNTPOINT" "$WORKDIR"
    if (( rc != 0 )); then
        printf '\n[FATAL] Test aborted with exit code %d\n' "$rc" | tee -a "$LOG"
    fi
    exit "$rc"
}
trap cleanup EXIT INT TERM

if (( EUID != 0 )); then
    exec sudo --preserve-env=TERM "$0" "$@"
fi

mkdir -p "$WORKDIR"
touch "$LOG"
exec > >(tee -a "$LOG") 2>&1

section() {
    printf '\n================================================================\n'
    printf '%s\n' "$1"
    printf '================================================================\n'
}

pass() {
    PASS=$((PASS + 1))
    printf '[PASS] %s\n' "$*"
}

fail() {
    FAIL=$((FAIL + 1))
    printf '[FAIL] %s\n' "$*"
}

known() {
    KNOWN=$((KNOWN + 1))
    printf '[NOT IMPLEMENTED] %s\n' "$*"
}

warn() {
    WARN=$((WARN + 1))
    printf '[WARN] %s\n' "$*"
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        printf '[FATAL] Required command is missing: %s\n' "$1" >&2
        exit 2
    }
}

timed() {
    local label="$1"
    shift
    local before after rc
    before="$(date +%s%N)"
    set +e
    "$@"
    rc=$?
    set -e
    after="$(date +%s%N)"
    printf '[TIME] %s: %.3f s (rc=%d)\n' "$label" \
        "$(awk -v a="$before" -v b="$after" 'BEGIN { printf "%.3f", (b-a)/1000000000 }')" \
        "$rc"
    return "$rc"
}

expected_unsupported() {
    local label="$1"
    shift
    set +e
    "$@" >/tmp/infiltratorfs-probe.out 2>/tmp/infiltratorfs-probe.err
    local rc=$?
    set -e
    if (( rc == 0 )); then
        pass "$label is implemented and succeeded"
        return 0
    fi
    local err
    err="$(tr '\n' ' ' </tmp/infiltratorfs-probe.err)"
    case "$err" in
        *"Operation not supported"*|*"Function not implemented"*|*"Invalid argument"*|*"Read-only file system"*)
            known "$label: ${err:-operation rejected}"
            ;;
        *)
            fail "$label failed unexpectedly (rc=$rc): ${err:-no stderr}"
            ;;
    esac
    return 0
}

section "InfiltratorFS partition 22 destructive native VFS test"
printf 'Target:       %s\n' "$TARGET"
printf 'Mount point:  %s\n' "$MOUNTPOINT"
printf 'Log:          %s\n' "$LOG"
printf 'Kernel:       %s\n' "$(uname -r)"
printf 'Started:      %s\n' "$(date --iso-8601=seconds)"

for cmd in \
    awk blockdev cmp cp cut date dd df findmnt grep head lsblk mkdir mkfs.infilfs \
    modinfo modprobe mount mountpoint od readlink rm sha256sum stat sync tail touch \
    truncate umount uname wc; do
    require_cmd "$cmd"
done

[[ -b "$TARGET" ]] || {
    printf '[FATAL] %s is not a block device.\n' "$TARGET" >&2
    exit 2
}

real_target="$(readlink -f "$TARGET")"
[[ "$real_target" == "$TARGET" ]] || {
    printf '[FATAL] Target resolved unexpectedly: %s -> %s\n' "$TARGET" "$real_target" >&2
    exit 2
}

parent_name="$(lsblk -ndo PKNAME "$TARGET" | head -n1)"
partno="$(lsblk -ndo PARTN "$TARGET" 2>/dev/null | head -n1 || true)"
[[ "/dev/${parent_name}" == "$EXPECTED_PARENT" ]] || {
    printf '[FATAL] Refusing target: parent is /dev/%s, expected %s.\n' \
        "$parent_name" "$EXPECTED_PARENT" >&2
    exit 2
}
if [[ -n "$partno" && "$partno" != "$EXPECTED_PARTNO" ]]; then
    printf '[FATAL] Refusing target: partition number is %s, expected 22.\n' "$partno" >&2
    exit 2
fi
[[ "$TARGET" == "${EXPECTED_PARENT}p${EXPECTED_PARTNO}" ]] || {
    printf '[FATAL] Safety gate mismatch: expected exactly %sp%s.\n' \
        "$EXPECTED_PARENT" "$EXPECTED_PARTNO" >&2
    exit 2
}

root_source="$(findmnt -rn -o SOURCE / | head -n1)"
[[ "$root_source" != "$TARGET" ]] || {
    printf '[FATAL] Refusing to destroy the root filesystem.\n' >&2
    exit 2
}

printf 'Partition size: %s bytes\n' "$(blockdev --getsize64 "$TARGET")"
lsblk -o NAME,PATH,SIZE,FSTYPE,LABEL,MOUNTPOINTS "$EXPECTED_PARENT"

if [[ "$CONFIRM_ARG" != "--destroy-partition-22" ]]; then
    printf '\nWARNING: ALL DATA ON %s WILL BE DESTROYED.\n' "$TARGET"
    printf 'Type exactly: ERASE /dev/mmcblk0p22\n> '
    read -r confirmation
    [[ "$confirmation" == "ERASE /dev/mmcblk0p22" ]] || {
        printf 'Cancelled.\n'
        exit 3
    }
fi

section "Preflight: unmount and native-driver verification"
while IFS= read -r mp; do
    [[ -n "$mp" ]] || continue
    printf 'Unmounting existing target mount: %s\n' "$mp"
    umount "$mp"
done < <(findmnt -rn -S "$TARGET" -o TARGET 2>/dev/null || true)

if findmnt -rn -t fuse.infilfs-fuse >/dev/null 2>&1; then
    fail "A legacy InfiltratorFS FUSE mount is still active"
else
    pass "No legacy InfiltratorFS FUSE mount is active"
fi

modprobe infiltratorfs
if grep -qw infiltratorfs /proc/filesystems; then
    pass "Native infiltratorfs filesystem is registered in /proc/filesystems"
else
    printf '[FATAL] Native infiltratorfs filesystem is not registered.\n' >&2
    exit 4
fi

printf 'Module file: %s\n' "$(modinfo -n infiltratorfs 2>/dev/null || echo unknown)"
printf 'Module version: %s\n' "$(modinfo -F version infiltratorfs 2>/dev/null || echo unspecified)"

section "Destructive format"
timed "mkfs.infilfs partition 22" mkfs.infilfs --force -L "$LABEL" "$TARGET"
sync
pass "Partition formatted as InfiltratorFS"

if command -v infilfs-inspect >/dev/null 2>&1; then
    timed "infilfs-inspect after format" infilfs-inspect "$TARGET" || fail "infilfs-inspect rejected newly formatted volume"
fi
if command -v infilfs-scrub >/dev/null 2>&1; then
    timed "initial unmounted scrub" infilfs-scrub "$TARGET" && pass "Initial scrub is clean" || fail "Initial scrub failed"
fi
if command -v fsck.infiltratorfs >/dev/null 2>&1; then
    timed "initial fsck.infiltratorfs" fsck.infiltratorfs -n "$TARGET" && pass "Initial fsck wrapper is clean" || fail "Initial fsck wrapper failed"
fi

section "Native read-write mount"
mkdir -p "$MOUNTPOINT"
timed "native mount" mount -t infiltratorfs -o rw "$TARGET" "$MOUNTPOINT"
MOUNTED=1

fstype="$(findmnt -rn -T "$MOUNTPOINT" -o FSTYPE)"
source="$(findmnt -rn -T "$MOUNTPOINT" -o SOURCE)"
options="$(findmnt -rn -T "$MOUNTPOINT" -o OPTIONS)"
[[ "$fstype" == "infiltratorfs" ]] && pass "Mounted through native VFS (fstype=infiltratorfs)" || fail "Wrong filesystem type: $fstype"
[[ "$source" == "$TARGET" ]] && pass "Mounted source is exactly partition 22" || fail "Mounted source mismatch: $source"
[[ ",$options," == *,rw,* ]] && pass "Mount is read-write" || fail "Mount is not read-write: $options"
[[ "$fstype" != fuse.* ]] && pass "Mount is not FUSE" || fail "Unexpected FUSE filesystem type: $fstype"

section "statfs / desktop capacity accounting"
read -r fs_type block_size blocks free_blocks avail_blocks < <(
    stat -f -c '%T %S %b %f %a' "$MOUNTPOINT"
)
printf 'statfs type=%s block_size=%s blocks=%s free=%s avail=%s\n' \
    "$fs_type" "$block_size" "$blocks" "$free_blocks" "$avail_blocks"
df -hT "$MOUNTPOINT"

if (( block_size == 4096 && blocks > 0 && free_blocks > 0 && free_blocks <= blocks )); then
    pass "statfs reports usable 4096-byte total/free block accounting (Nautilus pie-chart prerequisite)"
else
    fail "statfs capacity accounting is invalid"
fi

device_bytes="$(blockdev --getsize64 "$TARGET")"
reported_bytes=$((block_size * blocks))
difference=$(( device_bytes > reported_bytes ? device_bytes - reported_bytes : reported_bytes - device_bytes ))
if (( difference <= 4096 )); then
    pass "statfs total capacity matches partition size"
else
    fail "statfs capacity differs from partition size by ${difference} bytes"
fi

FREE_BEFORE="$free_blocks"

section "Creation / modification / change timestamps"
timestamp_file="$MOUNTPOINT/timestamp-test.txt"
printf 'created at %s\n' "$(date --iso-8601=ns)" >"$timestamp_file"
sync
birth_epoch="$(stat -c '%W' "$timestamp_file")"
birth_human="$(stat -c '%w' "$timestamp_file")"
mtime_before="$(stat -c '%Y' "$timestamp_file")"
ctime_before="$(stat -c '%Z' "$timestamp_file")"
printf 'Birth:  %s (%s)\n' "$birth_human" "$birth_epoch"
printf 'Modify: %s\n' "$(stat -c '%y' "$timestamp_file")"
printf 'Change: %s\n' "$(stat -c '%z' "$timestamp_file")"
if (( birth_epoch > 0 )); then
    pass "STATX_BTIME / creation time is exposed"
else
    fail "Creation time is missing (Nautilus would show Created: —)"
fi

sleep 1
printf 'second write\n' >>"$timestamp_file"
sync
mtime_after="$(stat -c '%Y' "$timestamp_file")"
ctime_after="$(stat -c '%Z' "$timestamp_file")"
(( mtime_after > mtime_before )) && pass "Modification time advances after write" || fail "Modification time did not advance"
(( ctime_after >= ctime_before )) && pass "Change time is non-regressing" || fail "Change time regressed"
[[ "$(stat -c '%W' "$timestamp_file")" == "$birth_epoch" ]] && pass "Birth time remains stable after modification" || fail "Birth time changed after write"

section "Inline and extent-backed read/write integrity"
printf 'InfiltratorFS native inline test %s\n' "$TIMESTAMP" >"$WORKDIR/inline.src"
timed "copy inline file" cp "$WORKDIR/inline.src" "$MOUNTPOINT/inline.txt"
sync
cmp -s "$WORKDIR/inline.src" "$MOUNTPOINT/inline.txt" && pass "Inline file round-trip matches byte-for-byte" || fail "Inline file data mismatch"

dd if=/dev/urandom of="$WORKDIR/extent-8m.src" bs=1M count=8 status=none
src_hash="$(sha256sum "$WORKDIR/extent-8m.src" | awk '{print $1}')"
timed "copy 8 MiB extent-backed file" cp "$WORKDIR/extent-8m.src" "$MOUNTPOINT/extent-8m.bin"
sync
dst_hash="$(sha256sum "$MOUNTPOINT/extent-8m.bin" | awk '{print $1}')"
[[ "$src_hash" == "$dst_hash" ]] && pass "8 MiB extent-backed SHA-256 matches" || fail "8 MiB extent-backed SHA-256 mismatch"

section "Large sequential write/read"
timed "write 512 MiB zero file" dd if=/dev/zero of="$MOUNTPOINT/large-512m.bin" bs=4M count=128 conv=fsync status=progress
large_size="$(stat -c '%s' "$MOUNTPOINT/large-512m.bin")"
[[ "$large_size" == "536870912" ]] && pass "512 MiB file size is correct" || fail "512 MiB file size incorrect: $large_size"
target_zero_hash="$(sha256sum "$MOUNTPOINT/large-512m.bin" | awk '{print $1}')"
expected_zero_hash="$(head -c 536870912 /dev/zero | sha256sum | awk '{print $1}')"
[[ "$target_zero_hash" == "$expected_zero_hash" ]] && pass "512 MiB sequential read hash matches" || fail "512 MiB sequential read hash mismatch"

section "Append semantics"
: >"$MOUNTPOINT/append.txt"
for i in $(seq 1 100); do
    printf 'line-%03d:%032d\n' "$i" "$i" >>"$MOUNTPOINT/append.txt"
done
sync
[[ "$(wc -l <"$MOUNTPOINT/append.txt")" == "100" ]] && pass "100 append operations preserved all records" || fail "Append line count mismatch"
grep -qx 'line-100:00000000000000000000000000000100' "$MOUNTPOINT/append.txt" && pass "Append tail content is correct" || fail "Append tail content mismatch"

section "Directory scaling / paged metadata"
mkdir "$MOUNTPOINT/many-files"
for i in $(seq -w 1 260); do
    printf 'file-%s\n' "$i" >"$MOUNTPOINT/many-files/file-$i.txt"
done
sync
many_count="$(find "$MOUNTPOINT/many-files" -maxdepth 1 -type f | wc -l)"
[[ "$many_count" == "260" ]] && pass "260-file directory crosses paged metadata/index boundaries successfully" || fail "Expected 260 files, found $many_count"
for i in 001 125 126 250 260; do
    expected="file-$i"
    actual="$(tr -d '\n' <"$MOUNTPOINT/many-files/file-$i.txt")"
    [[ "$actual" == "$expected" ]] || fail "Paged-directory sample file-$i content mismatch"
done
pass "Paged-directory sample contents are readable"

section "Nested directories and UTF-8 names"
mkdir -p "$MOUNTPOINT/a/b/c/d/e"
printf 'deep\n' >"$MOUNTPOINT/a/b/c/d/e/deep.txt"
[[ "$(cat "$MOUNTPOINT/a/b/c/d/e/deep.txt")" == "deep" ]] && pass "Nested directory traversal works" || fail "Nested directory traversal failed"

unicode_dir="$MOUNTPOINT/Unicode-Ä-Ж-漢字-🙂"
mkdir "$unicode_dir"
printf 'UTF-8 payload\n' >"$unicode_dir/naïve-文件-🙂.txt"
[[ -f "$unicode_dir/naïve-文件-🙂.txt" ]] && pass "UTF-8 directory and filename round-trip" || fail "UTF-8 namespace round-trip failed"

long255="$(head -c 255 < /dev/zero | tr '\0' a)"
printf 'long-name\n' >"$MOUNTPOINT/$long255"
[[ -f "$MOUNTPOINT/$long255" ]] && pass "255-byte filename succeeds" || fail "255-byte filename failed"

long256="${long255}b"
set +e
printf 'too-long\n' >"$MOUNTPOINT/$long256" 2>"$WORKDIR/long256.err"
rc=$?
set -e
if (( rc != 0 )); then
    pass "256-byte filename is correctly rejected"
else
    fail "256-byte filename unexpectedly succeeded"
fi

section "Sparse/truncate and random-overwrite capability probes"
# Current native VFS may not yet support large truncate or non-append overwrite.
# Probe them because they matter to real applications, but report EOPNOTSUPP as
# an implementation gap rather than confusing it with silent corruption.
expected_unsupported "large truncate/sparse-file growth" \
    truncate -s 134217728 "$MOUNTPOINT/sparse-128m.bin"

dd if=/dev/urandom of="$WORKDIR/overwrite.src" bs=1M count=8 status=none
cp "$WORKDIR/overwrite.src" "$MOUNTPOINT/overwrite.bin"
dd if=/dev/urandom of="$WORKDIR/patch.bin" bs=1M count=1 status=none
expected_unsupported "in-place random overwrite" \
    dd if="$WORKDIR/patch.bin" of="$MOUNTPOINT/overwrite.bin" bs=1M seek=3 count=1 conv=notrunc status=none

section "Namespace mutation capability probes"
printf 'rename probe\n' >"$MOUNTPOINT/rename-source.txt"
expected_unsupported "rename" mv "$MOUNTPOINT/rename-source.txt" "$MOUNTPOINT/rename-destination.txt"

printf 'unlink probe\n' >"$MOUNTPOINT/unlink-probe.txt"
expected_unsupported "unlink/delete file" rm "$MOUNTPOINT/unlink-probe.txt"

mkdir "$MOUNTPOINT/rmdir-probe"
expected_unsupported "remove directory" rmdir "$MOUNTPOINT/rmdir-probe"

printf 'hardlink probe\n' >"$MOUNTPOINT/hardlink-source.txt"
expected_unsupported "hard link creation" ln "$MOUNTPOINT/hardlink-source.txt" "$MOUNTPOINT/hardlink-copy.txt"

expected_unsupported "symbolic link creation" ln -s "inline.txt" "$MOUNTPOINT/symlink-probe"

expected_unsupported "chmod metadata update" chmod 0600 "$MOUNTPOINT/inline.txt"

section "Free-space accounting after writes"
sync
read -r block_size_after blocks_after free_after avail_after < <(
    stat -f -c '%S %b %f %a' "$MOUNTPOINT"
)
printf 'Before free blocks: %s\nAfter free blocks:  %s\n' "$FREE_BEFORE" "$free_after"
if (( free_after < FREE_BEFORE )); then
    pass "statfs free-space decreases after data allocation"
else
    fail "statfs free-space did not decrease after substantial writes"
fi
(( blocks_after == blocks )) && pass "statfs total capacity remains stable" || fail "statfs total block count changed unexpectedly"

section "fsync, unmount, offline integrity check"
sync
timed "unmount after write workload" umount "$MOUNTPOINT"
MOUNTED=0

if command -v infilfs-scrub >/dev/null 2>&1; then
    timed "post-write offline scrub" infilfs-scrub "$TARGET" && pass "Post-write scrub is clean" || fail "Post-write scrub found corruption"
fi
if command -v fsck.infiltratorfs >/dev/null 2>&1; then
    timed "post-write fsck.infiltratorfs" fsck.infiltratorfs -n "$TARGET" && pass "Post-write fsck wrapper is clean" || fail "Post-write fsck wrapper failed"
fi

section "Read-only remount persistence verification"
timed "read-only remount" mount -t infiltratorfs -o ro "$TARGET" "$MOUNTPOINT"
MOUNTED=1
ro_opts="$(findmnt -rn -T "$MOUNTPOINT" -o OPTIONS)"
[[ ",$ro_opts," == *,ro,* ]] && pass "Read-only remount is actually read-only" || fail "Read-only remount options are wrong: $ro_opts"

cmp -s "$WORKDIR/inline.src" "$MOUNTPOINT/inline.txt" && pass "Inline data survives unmount/remount" || fail "Inline data changed across remount"
[[ "$(sha256sum "$MOUNTPOINT/extent-8m.bin" | awk '{print $1}')" == "$src_hash" ]] && pass "Extent-backed data survives unmount/remount" || fail "Extent-backed data changed across remount"
[[ "$(sha256sum "$MOUNTPOINT/large-512m.bin" | awk '{print $1}')" == "$expected_zero_hash" ]] && pass "512 MiB file survives unmount/remount" || fail "512 MiB file changed across remount"
[[ "$(stat -c '%W' "$MOUNTPOINT/timestamp-test.txt")" == "$birth_epoch" ]] && pass "Birth time survives unmount/remount" || fail "Birth time changed/lost across remount"
[[ "$(find "$MOUNTPOINT/many-files" -maxdepth 1 -type f | wc -l)" == "260" ]] && pass "Paged directory survives unmount/remount" || fail "Paged directory lost entries across remount"

set +e
printf 'must fail\n' >"$MOUNTPOINT/readonly-write-test.txt" 2>"$WORKDIR/ro.err"
rc=$?
set -e
(( rc != 0 )) && pass "Write is rejected on read-only mount" || fail "Write unexpectedly succeeded on read-only mount"

timed "final unmount" umount "$MOUNTPOINT"
MOUNTED=0

if command -v infilfs-scrub >/dev/null 2>&1; then
    timed "final offline scrub" infilfs-scrub "$TARGET" && pass "Final scrub is clean" || fail "Final scrub found corruption"
fi

section "Kernel diagnostics"
if command -v journalctl >/dev/null 2>&1; then
    journalctl -k --since "@${START_EPOCH}" --no-pager 2>/dev/null | \
        grep -Ei 'infiltratorfs|I/O error|corrupt|BUG:|WARNING:|Oops:|Call Trace|general protection fault|kernel panic' || true
else
    dmesg | tail -n 250 | \
        grep -Ei 'infiltratorfs|I/O error|corrupt|BUG:|WARNING:|Oops:|Call Trace|general protection fault|kernel panic' || true
fi

section "Summary"
elapsed=$(( $(date +%s) - START_EPOCH ))
printf 'Passed:             %d\n' "$PASS"
printf 'Failed:             %d\n' "$FAIL"
printf 'Not implemented:    %d\n' "$KNOWN"
printf 'Warnings:           %d\n' "$WARN"
printf 'Elapsed:             %d seconds\n' "$elapsed"
printf 'Full log:            %s\n' "$LOG"
printf 'Target tested:       %s\n' "$TARGET"
printf 'Filesystem path:     native kernel VFS only\n'

if (( FAIL > 0 )); then
    printf '\nRESULT: FAIL — %d implemented-behaviour/integrity checks failed.\n' "$FAIL"
    exit 1
fi

printf '\nRESULT: PASS for implemented native operations.'
if (( KNOWN > 0 )); then
    printf ' %d capability probe(s) remain not implemented.\n' "$KNOWN"
else
    printf ' All probed capabilities are implemented.\n'
fi
exit 0
