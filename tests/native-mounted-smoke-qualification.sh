#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Bounded real-VFS smoke gate for every main commit. Deep mounted endurance,
# fragmentation and 512 MiB transaction work is a separate manual qualification.
set -Eeuo pipefail
[[ $# == 2 && $EUID == 0 ]] || {
    echo 'usage (as root): native-mounted-smoke-qualification.sh BUILD MODULE' >&2
    exit 2
}
build="$(realpath "$1")"
module="$(realpath "$2")"
work="$(mktemp -d)"
image="$work/native.img"
mnt="$work/mnt"
loop=""
mounted=0
loaded=0
cleanup() {
    local rc=$?
    trap - EXIT
    set +e
    if (( mounted )); then timeout -k 2s 6s umount "$mnt"; fi
    if [[ -n "$loop" ]]; then timeout -k 2s 6s losetup -d "$loop"; fi
    if (( loaded )); then timeout -k 2s 6s rmmod infiltratorfs; fi
    rm -rf -- "$work"
    exit "$rc"
}
trap cleanup EXIT
mkdir "$mnt"
truncate -s 64M "$image"
"$build/mkfs.infilfs" --force -L NativeSmoke "$image" >/dev/null
if ! grep -q '^infiltratorfs ' /proc/modules; then
    insmod "$module"
    loaded=1
fi
loop="$(losetup --find --show "$image")"
mount -t infiltratorfs -o rw "$loop" "$mnt"
mounted=1

# A fault-shortened full-folio overwrite must retain the untouched suffix.
cc -std=gnu11 -O2 -Wall -Wextra -Werror \
    -o "$work/short-write" tests/native-short-write-qualification.c
echo 'Native smoke: partial user-copy and post-fault readback'
timeout -k 3s 30s "$work/short-write" "$mnt"
echo 'Native smoke: statfs capacity/name limit and root ownership'
test "$(timeout -k 3s 20s stat -f -c %l "$mnt")" = 1023
# mkfs preserves the calling desktop user's ownership under sudo.
test "$(timeout -k 3s 20s stat -c '%u:%g' "$mnt")" = "${SUDO_UID:-0}:${SUDO_GID:-0}"

# The on-disk 1023-byte component limit must be usable from the native VFS,
# while 1024 bytes must fail specifically as ENAMETOOLONG.
echo 'Native smoke: 1023-byte create/fsync/list/unlink and 1024-byte rejection'
timeout -k 3s 30s python3 - "$mnt" <<'PY'
import errno
import os
import sys

root = sys.argv[1]
legal = 'n' * 1023
path = os.path.join(root, legal)
fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
try:
    assert os.write(fd, b'long-name-native\n') == 17
    os.fsync(fd)
    assert os.pread(fd, 17, 0) == b'long-name-native\n'
finally:
    os.close(fd)
assert legal in os.listdir(root)
try:
    fd = os.open(path + 'n', os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
except OSError as exc:
    assert exc.errno == errno.ENAMETOOLONG, exc
else:
    os.close(fd)
    raise AssertionError('1024-byte name accepted')
os.unlink(path)
PY
echo 'Native smoke: sync, clean unmount and independent metadata scrub'
timeout -k 3s 30s sync -f "$mnt"
timeout -k 3s 30s umount "$mnt"
mounted=0
timeout -k 3s 30s losetup -d "$loop"
loop=""
"$build/fsck.infiltratorfs" --scrub -n "$image" >"$work/scrub.out" || {
    cat "$work/scrub.out" >&2
    exit 1
}
grep -Fq 'Result:              CLEAN' "$work/scrub.out"
echo 'Native bounded mounted qualification passed.'
