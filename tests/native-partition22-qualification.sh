#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# InfiltratorFS destructive native VFS qualification for Shannon's dedicated
# /dev/mmcblk0p22 test partition.
#
# THIS SCRIPT DESTROYS /dev/mmcblk0p22.
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
    printf '\n================================================================\n%s\n================================================================\n' "$1"
}
pass() { PASS=$((PASS + 1)); printf '[PASS] %s\n' "$*"; }
fail() { FAIL=$((FAIL + 1)); printf '[FAIL] %s\n' "$*"; }
warn() { WARN=$((WARN + 1)); printf '[WARN] %s\n' "$*"; }
require_cmd() { command -v "$1" >/dev/null 2>&1 || { printf '[FATAL] Required command is missing: %s\n' "$1" >&2; exit 2; }; }

timed() {
    local label="$1" before after rc
    shift
    before="$(date +%s%N)"
    set +e
    "$@"
    rc=$?
    set -e
    after="$(date +%s%N)"
    printf '[TIME] %s: %.3f s (rc=%d)\n' "$label" \
        "$(awk -v a="$before" -v b="$after" 'BEGIN { printf "%.3f", (b-a)/1000000000 }')" "$rc"
    return "$rc"
}

section "InfiltratorFS partition 22 destructive native VFS test"
printf 'Target:       %s\nMount point:  %s\nLog:          %s\nKernel:       %s\nStarted:      %s\n' \
    "$TARGET" "$MOUNTPOINT" "$LOG" "$(uname -r)" "$(date --iso-8601=seconds)"

for cmd in awk blockdev cat chmod cmp cp date dd df fallocate find findmnt grep head \
    infilfs-tool ln lsblk mkdir mkfifo mkfs.infilfs mknod modinfo modprobe mount \
    mv python3 readlink rm rmdir seq sha256sum sleep stat sync touch tr truncate \
    umount uname wc; do
    require_cmd "$cmd"
done

[[ -b "$TARGET" ]] || { printf '[FATAL] %s is not a block device.\n' "$TARGET" >&2; exit 2; }
real_target="$(readlink -f "$TARGET")"
[[ "$real_target" == "$TARGET" ]] || { printf '[FATAL] Target resolved unexpectedly: %s -> %s\n' "$TARGET" "$real_target" >&2; exit 2; }

# lsblk's column formatter may pad numeric PARTN values. Normalize both parent
# and partition number before the destructive safety comparison.
parent_name="$(lsblk -ndo PKNAME "$TARGET" | head -n1 | tr -d '[:space:]')"
partno="$(lsblk -ndo PARTN "$TARGET" 2>/dev/null | head -n1 | tr -d '[:space:]' || true)"
[[ "/dev/${parent_name}" == "$EXPECTED_PARENT" ]] || { printf '[FATAL] Refusing target: parent is /dev/%s, expected %s.\n' "$parent_name" "$EXPECTED_PARENT" >&2; exit 2; }
if [[ -n "$partno" && "$partno" != "$EXPECTED_PARTNO" ]]; then
    printf '[FATAL] Refusing target: partition number is %s, expected %s.\n' "$partno" "$EXPECTED_PARTNO" >&2
    exit 2
fi
[[ "$TARGET" == "${EXPECTED_PARENT}p${EXPECTED_PARTNO}" ]] || { printf '[FATAL] Safety gate mismatch.\n' >&2; exit 2; }
root_source="$(findmnt -rn -o SOURCE / | head -n1)"
[[ "$root_source" != "$TARGET" ]] || { printf '[FATAL] Refusing to destroy the root filesystem.\n' >&2; exit 2; }

printf 'Partition size: %s bytes\n' "$(blockdev --getsize64 "$TARGET")"
lsblk -o NAME,PATH,SIZE,FSTYPE,LABEL,MOUNTPOINTS "$EXPECTED_PARENT"

if [[ "$CONFIRM_ARG" != "--destroy-partition-22" ]]; then
    printf '\nWARNING: ALL DATA ON %s WILL BE DESTROYED.\nType exactly: ERASE /dev/mmcblk0p22\n> ' "$TARGET"
    read -r confirmation
    [[ "$confirmation" == "ERASE /dev/mmcblk0p22" ]] || { printf 'Cancelled.\n'; exit 3; }
fi

section "Preflight: unmount and native-driver verification"
while IFS= read -r mp; do
    [[ -n "$mp" ]] || continue
    printf 'Unmounting existing target mount: %s\n' "$mp"
    umount "$mp"
done < <(findmnt -rn -S "$TARGET" -o TARGET 2>/dev/null || true)

if findmnt -rn -t fuse.infilfs-fuse >/dev/null 2>&1; then fail "A legacy InfiltratorFS FUSE mount is still active"; else pass "No legacy InfiltratorFS FUSE mount is active"; fi
modprobe infiltratorfs
grep -qw infiltratorfs /proc/filesystems || { printf '[FATAL] Native infiltratorfs filesystem is not registered.\n' >&2; exit 4; }
pass "Native infiltratorfs filesystem is registered in /proc/filesystems"
printf 'Module file: %s\n' "$(modinfo -n infiltratorfs 2>/dev/null || echo unknown)"
printf 'Module version: %s\n' "$(modinfo -F version infiltratorfs 2>/dev/null || echo unspecified)"

section "Destructive format"
timed "mkfs.infilfs partition 22" mkfs.infilfs --force -L "$LABEL" "$TARGET"
sync
pass "Partition formatted as InfiltratorFS"
if command -v infilfs-inspect >/dev/null 2>&1; then timed "infilfs-inspect after format" infilfs-inspect "$TARGET" || fail "infilfs-inspect rejected newly formatted volume"; fi
if command -v infilfs-scrub >/dev/null 2>&1; then timed "initial unmounted scrub" infilfs-scrub "$TARGET" && pass "Initial scrub is clean" || fail "Initial scrub failed"; fi
if command -v fsck.infiltratorfs >/dev/null 2>&1; then timed "initial fsck.infiltratorfs" fsck.infiltratorfs -n "$TARGET" && pass "Initial fsck wrapper is clean" || fail "Initial fsck wrapper failed"; fi

printf 'snapshot-before-native\n' >"$WORKDIR/snapshot-before.txt"
infilfs-tool "$TARGET" put "$WORKDIR/snapshot-before.txt" /snapshot-live.txt
infilfs-tool "$TARGET" snapshot-create before-native-rw
pass "Retained snapshot created before native writable mount"

section "Native read-write mount"
mkdir -p "$MOUNTPOINT"
timed "native mount" mount -t infiltratorfs -o rw "$TARGET" "$MOUNTPOINT"
MOUNTED=1
fstype="$(findmnt -rn -T "$MOUNTPOINT" -o FSTYPE)"
source="$(findmnt -rn -T "$MOUNTPOINT" -o SOURCE)"
options="$(findmnt -rn -T "$MOUNTPOINT" -o OPTIONS)"
[[ "$fstype" == infiltratorfs ]] && pass "Mounted through native VFS (fstype=infiltratorfs)" || fail "Wrong filesystem type: $fstype"
[[ "$source" == "$TARGET" ]] && pass "Mounted source is exactly partition 22" || fail "Mounted source mismatch: $source"
[[ ",$options," == *,rw,* ]] && pass "Mount is read-write" || fail "Mount is not read-write: $options"
[[ "$fstype" != fuse.* ]] && pass "Mount is not FUSE" || fail "Unexpected FUSE filesystem type: $fstype"
printf 'snapshot-after-native\n' >"$MOUNTPOINT/snapshot-live.txt"

section "statfs / desktop capacity accounting"
# GNU stat may render an unknown magic as two words, for example
# "UNKNOWN (0x494e4653)". Read the human-readable type separately so it cannot
# shift the numeric fields and create false capacity failures.
fs_type="$(stat -f -c '%T' "$MOUNTPOINT")"
read -r block_size blocks free_blocks avail_blocks < <(stat -f -c '%S %b %f %a' "$MOUNTPOINT")
printf 'statfs type=%s block_size=%s blocks=%s free=%s avail=%s\n' "$fs_type" "$block_size" "$blocks" "$free_blocks" "$avail_blocks"
df -hT "$MOUNTPOINT"
if (( block_size == 4096 && blocks > 0 && free_blocks > 0 && free_blocks <= blocks )); then pass "statfs reports usable 4096-byte total/free block accounting"; else fail "statfs capacity accounting is invalid"; fi
device_bytes="$(blockdev --getsize64 "$TARGET")"
reported_bytes=$((block_size * blocks))
difference=$(( device_bytes > reported_bytes ? device_bytes - reported_bytes : reported_bytes - device_bytes ))
(( difference <= 4096 )) && pass "statfs total capacity matches partition size" || fail "statfs capacity differs from partition size by ${difference} bytes"
FREE_BEFORE="$free_blocks"
TOTAL_BLOCKS_BEFORE="$blocks"

section "Creation / modification / change timestamps"
timestamp_file="$MOUNTPOINT/timestamp-test.txt"
printf 'created at %s\n' "$(date --iso-8601=ns)" >"$timestamp_file"
sync
birth_epoch="$(stat -c '%W' "$timestamp_file")"
birth_human="$(stat -c '%w' "$timestamp_file")"
mtime_before="$(stat -c '%Y' "$timestamp_file")"
ctime_before="$(stat -c '%Z' "$timestamp_file")"
printf 'Birth:  %s (%s)\nModify: %s\nChange: %s\n' "$birth_human" "$birth_epoch" "$(stat -c '%y' "$timestamp_file")" "$(stat -c '%z' "$timestamp_file")"
(( birth_epoch > 0 )) && pass "STATX_BTIME / creation time is exposed" || fail "Creation time is missing"
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
write_before="$(date +%s%N)"
dd if=/dev/zero of="$MOUNTPOINT/large-512m.bin" bs=4M count=128 conv=fsync status=progress
write_after="$(date +%s%N)"
write_seconds="$(awk -v a="$write_before" -v b="$write_after" 'BEGIN { printf "%.3f", (b-a)/1000000000 }')"
printf '[TIME] write 512 MiB zero file: %s s\n' "$write_seconds"
printf '[PERF] sequential write: %.2f MiB/s\n' "$(awk -v s="$write_seconds" 'BEGIN {print 512/s}')"
large_size="$(stat -c '%s' "$MOUNTPOINT/large-512m.bin")"
[[ "$large_size" == 536870912 ]] && pass "512 MiB file size is correct" || fail "512 MiB file size incorrect: $large_size"
read_before="$(date +%s%N)"
target_zero_hash="$(sha256sum "$MOUNTPOINT/large-512m.bin" | awk '{print $1}')"
read_after="$(date +%s%N)"
read_seconds="$(awk -v a="$read_before" -v b="$read_after" 'BEGIN { printf "%.3f", (b-a)/1000000000 }')"
printf '[PERF] verified sequential read: %.2f MiB/s\n' "$(awk -v s="$read_seconds" 'BEGIN {print 512/s}')"
expected_zero_hash="$(head -c 536870912 /dev/zero | sha256sum | awk '{print $1}')"
[[ "$target_zero_hash" == "$expected_zero_hash" ]] && pass "512 MiB sequential read hash matches" || fail "512 MiB sequential read hash mismatch"

section "Append semantics"
: >"$MOUNTPOINT/append.txt"
for i in $(seq 1 100); do printf 'line-%03d:%032d\n' "$i" "$i" >>"$MOUNTPOINT/append.txt"; done
sync
[[ "$(wc -l <"$MOUNTPOINT/append.txt")" == 100 ]] && pass "100 append operations preserved all records" || fail "Append line count mismatch"
grep -qx 'line-100:00000000000000000000000000000100' "$MOUNTPOINT/append.txt" && pass "Append tail content is correct" || fail "Append tail content mismatch"

section "Directory scaling / paged metadata"
mkdir "$MOUNTPOINT/many-files"
[[ "$(stat -c '%b' "$MOUNTPOINT/many-files")" == 8 ]] && pass "Empty directory reports its allocated metadata block" || fail "Empty directory allocation reporting is incorrect"
for i in $(seq -w 1 260); do printf 'file-%s\n' "$i" >"$MOUNTPOINT/many-files/file-$i.txt"; done
sync
[[ "$(find "$MOUNTPOINT/many-files" -maxdepth 1 -type f | wc -l)" == 260 ]] && pass "260-file directory crosses paged metadata/index boundaries successfully" || fail "260-file directory count mismatch"
(( $(stat -c '%b' "$MOUNTPOINT/many-files") > 8 )) && pass "Paged directory reports head and page allocation" || fail "Paged directory allocation reporting did not grow"
for i in 001 125 126 250 260; do [[ "$(tr -d '\n' <"$MOUNTPOINT/many-files/file-$i.txt")" == "file-$i" ]] || fail "Paged-directory sample file-$i content mismatch"; done
pass "Paged-directory sample contents are readable"

section "Nested directories and UTF-8 names"
mkdir -p "$MOUNTPOINT/a/b/c/d/e"
printf 'deep\n' >"$MOUNTPOINT/a/b/c/d/e/deep.txt"
[[ "$(cat "$MOUNTPOINT/a/b/c/d/e/deep.txt")" == deep ]] && pass "Nested directory traversal works" || fail "Nested directory traversal failed"
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
(( rc != 0 )) && pass "256-byte filename is correctly rejected" || fail "256-byte filename unexpectedly succeeded"

section "Sparse/truncate and random-overwrite capability probes"
truncate -s 134217728 "$MOUNTPOINT/sparse-128m.bin"
[[ "$(stat -c '%s' "$MOUNTPOINT/sparse-128m.bin")" == 134217728 ]] && pass "Large truncate/sparse-file growth works" || fail "Large truncate/sparse-file growth returned the wrong size"
(( $(stat -c '%b' "$MOUNTPOINT/sparse-128m.bin") == 0 )) && pass "Sparse growth consumes no data blocks" || fail "Sparse growth unexpectedly allocated data blocks"
dd if=/dev/urandom of="$WORKDIR/overwrite.src" bs=1M count=8 status=none
cp "$WORKDIR/overwrite.src" "$MOUNTPOINT/overwrite.bin"
dd if=/dev/urandom of="$WORKDIR/patch.bin" bs=1M count=1 status=none
dd if="$WORKDIR/patch.bin" of="$MOUNTPOINT/overwrite.bin" bs=1M seek=3 count=1 conv=notrunc status=none
dd if="$WORKDIR/patch.bin" of="$WORKDIR/overwrite.src" bs=1M seek=3 count=1 conv=notrunc status=none
cmp -s "$WORKDIR/overwrite.src" "$MOUNTPOINT/overwrite.bin" && pass "In-place random overwrite preserves surrounding data" || fail "In-place random overwrite content mismatch"
truncate -s 536870912 "$WORKDIR/random-expected.bin"
python3 - "$WORKDIR/random-expected.bin" "$MOUNTPOINT/large-512m.bin" <<'PY'
import os, random, sys, time
expected_path, mounted_path = sys.argv[1:]
rng = random.Random(0x1F51A7E)
expected_fd = os.open(expected_path, os.O_RDWR)
mounted_fd = os.open(mounted_path, os.O_RDWR)
start = time.perf_counter()
try:
    for iteration in range(4000):
        offset = rng.randrange(536870912 // 4096) * 4096
        payload = bytes(((iteration * 17 + i * 31 + 9) & 0xff) for i in range(4096))
        assert os.pwrite(expected_fd, payload, offset) == len(payload)
        assert os.pwrite(mounted_fd, payload, offset) == len(payload)
        if iteration % 50 == 49:
            os.fsync(mounted_fd)
    os.fsync(expected_fd); os.fsync(mounted_fd)
finally:
    os.close(mounted_fd); os.close(expected_fd)
elapsed = time.perf_counter() - start
print(f"[PERF] random 4 KiB overwrite: {4000/elapsed:.2f} IOPS ({elapsed:.3f} s)")
PY
random_expected_hash="$(sha256sum "$WORKDIR/random-expected.bin" | awk '{print $1}')"
[[ "$(sha256sum "$MOUNTPOINT/large-512m.bin" | awk '{print $1}')" == "$random_expected_hash" ]] && pass "4,000 mounted random 4 KiB overwrites match the reference image" || fail "4,000-write mounted random-overwrite hash mismatch"
python3 - "$MOUNTPOINT/fsync-publications.bin" <<'PY'
import os, statistics, sys, time
fd = os.open(sys.argv[1], os.O_CREAT | os.O_RDWR | os.O_EXCL, 0o600)
lat=[]
try:
    for iteration in range(200):
        payload = bytes(((iteration * 23 + i * 7 + 3) & 0xff) for i in range(4096))
        assert os.pwrite(fd, payload, iteration * 4096) == len(payload)
        t=time.perf_counter_ns(); os.fsync(fd); lat.append((time.perf_counter_ns()-t)/1e6)
finally: os.close(fd)
lat.sort()
print(f"[PERF] fsync latency: mean={statistics.mean(lat):.3f}ms p50={lat[99]:.3f}ms p95={lat[189]:.3f}ms p99={lat[197]:.3f}ms max={lat[-1]:.3f}ms")
PY
[[ "$(stat -c '%s' "$MOUNTPOINT/fsync-publications.bin")" == 819200 ]] && pass "200 explicit mounted fsync publications complete" || fail "Repeated fsync publication file has wrong size"
fallocate -l 16M "$MOUNTPOINT/preallocated.bin"
before_punch="$(stat -c '%b' "$MOUNTPOINT/preallocated.bin")"
fallocate -p -o 4M -l 8M "$MOUNTPOINT/preallocated.bin"
after_punch="$(stat -c '%b' "$MOUNTPOINT/preallocated.bin")"
(( before_punch >= 32768 )) && pass "Normal fallocate allocates blocks" || fail "Normal fallocate did not allocate requested space"
(( after_punch < before_punch )) && pass "Hole punching releases data blocks" || fail "Hole punching did not release data blocks"

section "Namespace mutation capability probes"
printf 'rename probe\n' >"$MOUNTPOINT/rename-source.txt"; mv "$MOUNTPOINT/rename-source.txt" "$MOUNTPOINT/rename-destination.txt"
[[ -f "$MOUNTPOINT/rename-destination.txt" ]] && pass "Rename succeeds" || fail "Rename lost destination"
printf 'unlink probe\n' >"$MOUNTPOINT/unlink-probe.txt"; rm "$MOUNTPOINT/unlink-probe.txt"
[[ ! -e "$MOUNTPOINT/unlink-probe.txt" ]] && pass "Unlink succeeds" || fail "Unlink left source"
mkdir "$MOUNTPOINT/rmdir-probe"; rmdir "$MOUNTPOINT/rmdir-probe"
[[ ! -e "$MOUNTPOINT/rmdir-probe" ]] && pass "rmdir succeeds" || fail "rmdir left directory"
printf 'hardlink probe\n' >"$MOUNTPOINT/hardlink-source.txt"; ln "$MOUNTPOINT/hardlink-source.txt" "$MOUNTPOINT/hardlink-copy.txt"
[[ "$(stat -c '%i' "$MOUNTPOINT/hardlink-source.txt")" == "$(stat -c '%i' "$MOUNTPOINT/hardlink-copy.txt")" ]] && pass "Hard link creation succeeds" || fail "Hard link inode mismatch"
ln -s hardlink-source.txt "$MOUNTPOINT/symlink-probe"
[[ "$(readlink "$MOUNTPOINT/symlink-probe")" == hardlink-source.txt ]] && pass "Symbolic link creation succeeds" || fail "Symbolic link target mismatch"
[[ "$(stat -c '%b' "$MOUNTPOINT/symlink-probe")" == 8 ]] && pass "Symlink reports its allocated metadata block" || fail "Symlink allocation reporting is incorrect"
chmod 000 "$MOUNTPOINT/hardlink-source.txt"; [[ "$(stat -c '%a' "$MOUNTPOINT/hardlink-source.txt")" == 0 ]] && pass "chmod metadata update succeeds" || fail "chmod mode 000 did not persist"

python3 - "$MOUNTPOINT" <<'PY'
import os, sys
root=sys.argv[1]
p=os.path.join(root,'open-parent'); m=os.path.join(root,'open-parent-moved')
os.mkdir(p); child=os.path.join(p,'child')
fd=os.open(child,os.O_CREAT|os.O_RDWR|os.O_EXCL,0o600)
os.write(fd,b'parent-before'); os.fsync(fd); os.rename(p,m)
os.lseek(fd,0,os.SEEK_END); os.write(fd,b'-after'); os.unlink(os.path.join(m,'child')); os.fsync(fd)
os.lseek(fd,0,os.SEEK_SET); assert os.read(fd,64)==b'parent-before-after'; os.close(fd); os.rmdir(m)
PY
pass "Open rename/replace/parent-rename descriptor semantics succeed"

section "Linux xattr, special-node and mmap parity"
python3 - "$MOUNTPOINT/inline.txt" <<'PY'
import os, sys
p=sys.argv[1]
os.setxattr(p,b'user.infiltratorfs-test',b'created',os.XATTR_CREATE)
os.setxattr(p,b'user.infiltratorfs-test',b'replaced',os.XATTR_REPLACE)
assert os.getxattr(p,b'user.infiltratorfs-test')==b'replaced'
os.setxattr(p,b'trusted.infiltratorfs-test',b'trusted')
assert os.getxattr(p,b'trusted.infiltratorfs-test')==b'trusted'
PY
pass "Persistent user and trusted xattr create/replace/read succeeds"
mkfifo "$MOUNTPOINT/native.fifo"
python3 - "$MOUNTPOINT/native.socket" <<'PY'
import socket,sys
s=socket.socket(socket.AF_UNIX)
try: s.bind(sys.argv[1])
finally: s.close()
PY
mknod "$MOUNTPOINT/native.char" c 1 3; mknod "$MOUNTPOINT/native.block" b 7 0
[[ -p "$MOUNTPOINT/native.fifo" && -S "$MOUNTPOINT/native.socket" && -c "$MOUNTPOINT/native.char" && -b "$MOUNTPOINT/native.block" ]] && pass "FIFO/socket/character/block nodes succeed" || fail "Special-node type mismatch"
python3 - "$MOUNTPOINT/mmap.bin" <<'PY'
import mmap,os,sys
p=sys.argv[1]; exp=bytearray((i*31+9)&255 for i in range(2*1024*1024))
with open(p,'wb') as f: f.write(exp); f.flush(); os.fsync(f.fileno())
with open(p,'r+b') as f:
    with mmap.mmap(f.fileno(),0,access=mmap.ACCESS_WRITE) as m:
        m[12345:12361]=b'native-mmap-pass'; exp[12345:12361]=b'native-mmap-pass'; m.flush()
    os.fsync(f.fileno())
with open(p,'rb') as f: assert f.read()==exp
PY
pass "Shared writable mmap persists through verified writeback"

section "Free-space accounting after writes"
sync
read -r block_size_after blocks_after free_after avail_after < <(stat -f -c '%S %b %f %a' "$MOUNTPOINT")
printf 'Before free blocks: %s\nAfter free blocks:  %s\n' "$FREE_BEFORE" "$free_after"
(( free_after < FREE_BEFORE )) && pass "statfs free-space decreases after data allocation" || fail "statfs free-space did not decrease after substantial writes"
(( blocks_after == TOTAL_BLOCKS_BEFORE )) && pass "statfs total capacity remains stable" || fail "statfs total block count changed unexpectedly"

section "fsync, unmount, offline integrity check"
sync
timed "unmount after write workload" umount "$MOUNTPOINT"; MOUNTED=0
[[ "$(infilfs-tool "$TARGET" cat /snapshot-live.txt)" == snapshot-after-native ]] && pass "Live generation changed while a snapshot was retained" || fail "Live snapshot test content mismatch"
[[ "$(infilfs-tool "$TARGET" snapshot-cat before-native-rw /snapshot-live.txt)" == snapshot-before-native ]] && pass "Retained snapshot preserved its original generation" || fail "Retained snapshot content changed"
if command -v infilfs-scrub >/dev/null 2>&1; then timed "post-write offline scrub" infilfs-scrub "$TARGET" && pass "Post-write scrub is clean" || fail "Post-write scrub found corruption"; fi
if command -v fsck.infiltratorfs >/dev/null 2>&1; then timed "post-write fsck.infiltratorfs" fsck.infiltratorfs -n "$TARGET" && pass "Post-write fsck wrapper is clean" || fail "Post-write fsck wrapper failed"; fi

section "Read-only remount persistence verification"
timed "read-only remount" mount -t infiltratorfs -o ro "$TARGET" "$MOUNTPOINT"; MOUNTED=1
ro_opts="$(findmnt -rn -T "$MOUNTPOINT" -o OPTIONS)"
[[ ",$ro_opts," == *,ro,* ]] && pass "Read-only remount is actually read-only" || fail "Read-only remount options are wrong: $ro_opts"
cmp -s "$WORKDIR/inline.src" "$MOUNTPOINT/inline.txt" && pass "Inline data survives unmount/remount" || fail "Inline data changed across remount"
[[ "$(sha256sum "$MOUNTPOINT/extent-8m.bin" | awk '{print $1}')" == "$src_hash" ]] && pass "Extent-backed data survives unmount/remount" || fail "Extent-backed data changed across remount"
[[ "$(sha256sum "$MOUNTPOINT/large-512m.bin" | awk '{print $1}')" == "$random_expected_hash" ]] && pass "512 MiB random-write result survives unmount/remount" || fail "512 MiB file changed across remount"
[[ "$(stat -c '%W' "$MOUNTPOINT/timestamp-test.txt")" == "$birth_epoch" ]] && pass "Birth time survives unmount/remount" || fail "Birth time changed/lost across remount"
[[ "$(find "$MOUNTPOINT/many-files" -maxdepth 1 -type f | wc -l)" == 260 ]] && pass "Paged directory survives unmount/remount" || fail "Paged directory lost entries across remount"
[[ "$(python3 -c 'import os,sys; print(os.getxattr(sys.argv[1], b"user.infiltratorfs-test").decode())' "$MOUNTPOINT/inline.txt")" == replaced ]] && pass "User xattr survives unmount/remount" || fail "User xattr changed across remount"
[[ "$(python3 -c 'import os,sys; print(os.getxattr(sys.argv[1], b"trusted.infiltratorfs-test").decode())' "$MOUNTPOINT/inline.txt")" == trusted ]] && pass "Trusted xattr survives unmount/remount" || fail "Trusted xattr changed across remount"
[[ "$(stat -c '%b' "$MOUNTPOINT/many-files")" -gt 8 && "$(stat -c '%b' "$MOUNTPOINT/symlink-probe")" == 8 ]] && pass "Directory/symlink allocation reporting survives remount" || fail "Directory/symlink allocation reporting changed across remount"
[[ "$(stat -c '%s' "$MOUNTPOINT/fsync-publications.bin")" == 819200 ]] && pass "Repeated fsync result survives unmount/remount" || fail "Repeated fsync result changed across remount"
[[ -p "$MOUNTPOINT/native.fifo" && -S "$MOUNTPOINT/native.socket" && -c "$MOUNTPOINT/native.char" && -b "$MOUNTPOINT/native.block" ]] && pass "Special-node types survive unmount/remount" || fail "Special-node type changed across remount"
python3 - "$MOUNTPOINT/mmap.bin" <<'PY'
import sys
exp=bytearray((i*31+9)&255 for i in range(2*1024*1024)); exp[12345:12361]=b'native-mmap-pass'
with open(sys.argv[1],'rb') as f: assert f.read()==exp
PY
pass "mmap writeback survives unmount/remount"
set +e
printf 'must fail\n' >"$MOUNTPOINT/readonly-write-test.txt" 2>"$WORKDIR/ro.err"
rc=$?
set -e
(( rc != 0 )) && pass "Write is rejected on read-only mount" || fail "Write unexpectedly succeeded on read-only mount"
timed "final unmount" umount "$MOUNTPOINT"; MOUNTED=0
if command -v infilfs-scrub >/dev/null 2>&1; then timed "final offline scrub" infilfs-scrub "$TARGET" && pass "Final scrub is clean" || fail "Final scrub found corruption"; fi

section "Kernel diagnostics"
if command -v journalctl >/dev/null 2>&1; then
    journalctl -k --since "@${START_EPOCH}" --no-pager 2>/dev/null | grep -Ei 'infiltratorfs|I/O error|corrupt|BUG:|WARNING:|Oops:|Call Trace|general protection fault|kernel panic' || true
else
    dmesg | tail -n 250 | grep -Ei 'infiltratorfs|I/O error|corrupt|BUG:|WARNING:|Oops:|Call Trace|general protection fault|kernel panic' || true
fi

section "Summary"
elapsed=$(( $(date +%s) - START_EPOCH ))
printf 'Passed:             %d\nFailed:             %d\nWarnings:           %d\nElapsed:             %d seconds\nFull log:            %s\nTarget tested:       %s\nFilesystem path:     native kernel VFS only\n' "$PASS" "$FAIL" "$WARN" "$elapsed" "$LOG" "$TARGET"
if (( FAIL > 0 )); then
    printf '\nRESULT: FAIL — %d implemented-behaviour/integrity checks failed.\n' "$FAIL"
    exit 1
fi
printf '\nRESULT: PASS — all migrated native capabilities passed.\n'
exit 0
