#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail

build="${1:?build directory required}"
module="${2:?kernel module required}"
quota="$build/infiltratorfs-quota"
work="$(mktemp -d)"
image="$work/quota.img"
mnt="$work/mnt"
loopdev=""
mounted=0
loaded=0

on_error() {
    local rc=$?
    printf 'native quota qualification: FAIL line=%s rc=%d command=%s\n' \
        "${BASH_LINENO[0]:-$LINENO}" "$rc" "$BASH_COMMAND" >&2
    return "$rc"
}
trap on_error ERR

cleanup() {
    set +e
    if [[ "$mounted" = 1 ]]; then sudo umount "$mnt" || true; fi
    if [[ -n "$loopdev" ]]; then sudo losetup -d "$loopdev" || true; fi
    if [[ "$loaded" = 1 ]]; then sudo rmmod infiltratorfs || true; fi
    rm -rf "$work"
}
trap cleanup EXIT

stage() {
    printf 'native quota qualification: STAGE %s\n' "$1" >&2
}

quota_cmd() {
    local label="$1"
    shift
    stage "$label"
    timeout --foreground 90s sudo "$quota" "$@"
}

field() {
    local key="$1"
    shift
    timeout --foreground 90s sudo "$quota" get "$@" |
        awk -F= -v key="$key" '$1 == key { print $2 }'
}

append_once() {
    local path="$1"
    local bytes="$2"
    sudo timeout --foreground 90s python3 - "$path" "$bytes" <<'PY'
import os
import sys

path = sys.argv[1]
size = int(sys.argv[2])
fd = os.open(path, os.O_WRONLY | os.O_APPEND)
try:
    payload = bytes((i * 17 + 11) & 0xff for i in range(size))
    written = os.write(fd, payload)
    if written != size:
        raise OSError(f"short append: {written} of {size}")
finally:
    os.close(fd)
PY
}

stage "format and mount"
mkdir -p "$mnt"
truncate -s 512M "$image"
"$build/mkfs.infilfs" --force -L NativeQuota "$image" >/dev/null

if ! grep -qw infiltratorfs /proc/filesystems; then
    sudo insmod "$module"
    loaded=1
fi
loopdev="$(sudo losetup --find --show "$image")"
sudo mount -t infiltratorfs -o rw "$loopdev" "$mnt"
mounted=1

# User byte quota: reject growth before data is written, then release usage on
# truncate so later growth can consume the returned allowance.
quota_cmd "set initial user byte quota" set "$mnt" user 0 6MiB 0 >/dev/null
stage "exercise user byte quota"
sudo dd if=/dev/urandom of="$mnt/user-bytes.bin" bs=1M count=4 status=none
if append_once "$mnt/user-bytes.bin" $((3 * 1024 * 1024)) \
        2>"$work/user-edquot"; then
    echo "user byte quota allowed an over-limit append" >&2
    exit 1
fi
grep -Eqi 'quota|Disk quota exceeded' "$work/user-edquot"
test "$(stat -c '%s' "$mnt/user-bytes.bin")" -eq $((4 * 1024 * 1024))
sudo truncate -s 1M "$mnt/user-bytes.bin"
sudo dd if=/dev/urandom of="$mnt/user-bytes.bin" bs=1M count=4 \
    oflag=append conv=notrunc status=none
test "$(stat -c '%s' "$mnt/user-bytes.bin")" -eq $((5 * 1024 * 1024))

# User object quota counts persistent objects rather than directory entries:
# a hard link must not consume a second object charge.
stage "exercise user object quota"
user_used="$(field used_objects "$mnt" user 0)"
quota_cmd "replace user quota with object limit" set "$mnt" user 0 0 "$((user_used + 1))" >/dev/null
sudo touch "$mnt/user-object-one"
sudo ln "$mnt/user-object-one" "$mnt/user-object-link"
if sudo touch "$mnt/user-object-two" 2>"$work/user-object-edquot"; then
    echo "user object quota allowed an extra persistent object" >&2
    exit 1
fi
grep -Eqi 'quota|Disk quota exceeded' "$work/user-object-edquot"
test "$(stat -c '%i' "$mnt/user-object-one")" = \
     "$(stat -c '%i' "$mnt/user-object-link")"
quota_cmd "clear user quota" clear "$mnt" user 0 >/dev/null

# Group object quota uses the same native accounting path.
stage "exercise group object quota"
quota_cmd "seed group quota" set "$mnt" group 0 0 1000000 >/dev/null
group_used="$(field used_objects "$mnt" group 0)"
quota_cmd "tighten group object quota" set "$mnt" group 0 0 "$((group_used + 1))" >/dev/null
sudo touch "$mnt/group-object-one"
if sudo touch "$mnt/group-object-two" 2>"$work/group-edquot"; then
    echo "group object quota allowed an extra persistent object" >&2
    exit 1
fi
grep -Eqi 'quota|Disk quota exceeded' "$work/group-edquot"
quota_cmd "clear group quota" clear "$mnt" group 0 >/dev/null

# chown transfers accounting. A file larger than the destination user's hard
# limit must stay with its original owner.
stage "exercise ownership transfer quota"
sudo dd if=/dev/urandom of="$mnt/chown-candidate.bin" bs=1M count=2 status=none
quota_cmd "set destination user quota" set "$mnt" user 12345 1MiB 0 >/dev/null
if sudo chown 12345:12345 "$mnt/chown-candidate.bin" \
        2>"$work/chown-edquot"; then
    echo "chown crossed a destination user quota" >&2
    exit 1
fi
grep -Eqi 'quota|Disk quota exceeded' "$work/chown-edquot"
test "$(stat -c '%u' "$mnt/chown-candidate.bin")" -eq 0
quota_cmd "clear destination user quota" clear "$mnt" user 12345 >/dev/null

# Project IDs are directory-tree quota domains and inherit through descendants.
stage "create project domains"
sudo mkdir "$mnt/project-a" "$mnt/project-b" "$mnt/outside"
quota_cmd "assign project 42" project-set "$mnt/project-a" 42 >/dev/null
sudo mkdir "$mnt/project-a/sub"
test "$(timeout --foreground 90s sudo "$quota" project-get "$mnt/project-a/sub" | \
        awk -F= '$1 == "effective_project_id" { print $2 }')" -eq 42

quota_cmd "set project 42 quota" set "$mnt" project 42 5MiB 32 >/dev/null
stage "exercise project byte and reflink quota"
sudo dd if=/dev/urandom of="$mnt/project-a/data.bin" bs=1M count=4 status=none
if append_once "$mnt/project-a/data.bin" $((2 * 1024 * 1024)) \
        2>"$work/project42-edquot"; then
    echo "project byte quota allowed an over-limit append" >&2
    exit 1
fi
grep -Eqi 'quota|Disk quota exceeded' "$work/project42-edquot"
test "$(stat -c '%s' "$mnt/project-a/data.bin")" -eq $((4 * 1024 * 1024))

# Reflink growth is logical-byte growth for quota purposes even though physical
# blocks are shared.
if sudo cp --reflink=always "$mnt/project-a/data.bin" \
        "$mnt/project-a/reflink-over-limit.bin" \
        2>"$work/reflink-edquot"; then
    echo "reflink growth crossed the project byte quota" >&2
    exit 1
fi
grep -Eqi 'quota|Disk quota exceeded' "$work/reflink-edquot"
sudo rm -f "$mnt/project-a/reflink-over-limit.bin"

# A hard link inside the same project remains one persistent object.
sudo ln "$mnt/project-a/data.bin" "$mnt/project-a/sub/same-project-link"
test "$(stat -c '%i' "$mnt/project-a/data.bin")" = \
     "$(stat -c '%i' "$mnt/project-a/sub/same-project-link")"

# Cross-directory rename is preflighted against the destination project. It
# must fail atomically rather than moving first and discovering overage later.
stage "exercise cross-project preflight"
quota_cmd "assign project 43" project-set "$mnt/project-b" 43 >/dev/null
quota_cmd "set project 43 quota" set "$mnt" project 43 1MiB 32 >/dev/null
if sudo ln "$mnt/project-a/data.bin" "$mnt/project-b/cross-project-link" \
        2>"$work/hardlink-exdev"; then
    echo "cross-project hard link unexpectedly succeeded" >&2
    exit 1
fi
grep -Eqi 'cross-device|Invalid cross-device link' "$work/hardlink-exdev"
test ! -e "$mnt/project-b/cross-project-link"

sudo mkdir "$mnt/outside/move-me"
sudo dd if=/dev/urandom of="$mnt/outside/move-me/payload.bin" \
    bs=1M count=2 status=none
if sudo mv "$mnt/outside/move-me" "$mnt/project-b/move-me" \
        2>"$work/rename-edquot"; then
    echo "cross-project rename crossed the destination project quota" >&2
    exit 1
fi
grep -Eqi 'quota|Disk quota exceeded' "$work/rename-edquot"
test -f "$mnt/outside/move-me/payload.bin"
test ! -e "$mnt/project-b/move-me"

# Changing a directory into an already-full project is also a transfer and must
# receive the same preflight treatment.
stage "exercise project reassignment preflight"
sudo mkdir "$mnt/outside/project-candidate"
sudo dd if=/dev/urandom of="$mnt/outside/project-candidate/payload.bin" \
    bs=1M count=2 status=none
if timeout --foreground 90s sudo "$quota" project-set \
        "$mnt/outside/project-candidate" 43 \
        >"$work/project-set.out" 2>"$work/project-set-edquot"; then
    echo "project assignment crossed the destination project quota" >&2
    exit 1
fi
grep -Eqi 'quota|Disk quota exceeded' "$work/project-set-edquot"
test "$(timeout --foreground 90s sudo "$quota" project-get \
        "$mnt/outside/project-candidate" | \
        awk -F= '$1 == "effective_project_id" { print $2 }')" -eq 0

# Deleting or replacing an empty project-root directory must remove the object
# mapping and release the project's object charge.
stage "exercise project-root deletion and replacement"
sudo mkdir "$mnt/project-delete"
quota_cmd "assign project 77" project-set "$mnt/project-delete" 77 >/dev/null
quota_cmd "set project 77 quota" set "$mnt" project 77 0 4 >/dev/null
test "$(field used_objects "$mnt" project 77)" -eq 1
sudo rmdir "$mnt/project-delete"
test "$(field used_objects "$mnt" project 77)" -eq 0

sudo mkdir "$mnt/project-replace" "$mnt/replacement-source"
quota_cmd "assign project 78" project-set "$mnt/project-replace" 78 >/dev/null
quota_cmd "set project 78 quota" set "$mnt" project 78 0 4 >/dev/null
test "$(field used_objects "$mnt" project 78)" -eq 1
sudo mv -T "$mnt/replacement-source" "$mnt/project-replace"
test "$(timeout --foreground 90s sudo "$quota" project-get "$mnt/project-replace" | \
        awk -F= '$1 == "effective_project_id" { print $2 }')" -eq 0
test "$(field used_objects "$mnt" project 78)" -eq 0

# Policy and project-root metadata are persistent, but volatile usage is
# deliberately rebuilt from authoritative objects after remount.
stage "exercise quota persistence across remount"
before42="$(field used_bytes "$mnt" project 42)"
sudo sync
sudo umount "$mnt"
mounted=0
"$build/infilfs-scrub" "$image" | grep -Fq 'Result:              CLEAN'

sudo mount -t infiltratorfs -o rw "$loopdev" "$mnt"
mounted=1
test "$(timeout --foreground 90s sudo "$quota" project-get "$mnt/project-a/sub" | \
        awk -F= '$1 == "effective_project_id" { print $2 }')" -eq 42
after42="$(field used_bytes "$mnt" project 42)"
test "$after42" -eq "$before42"
if append_once "$mnt/project-a/data.bin" $((2 * 1024 * 1024)) \
        2>"$work/remount-edquot"; then
    echo "persisted project quota was not enforced after remount" >&2
    exit 1
fi
grep -Eqi 'quota|Disk quota exceeded' "$work/remount-edquot"
test "$(stat -c '%s' "$mnt/project-a/data.bin")" -eq $((4 * 1024 * 1024))

stage "final unmount and scrub"
sudo umount "$mnt"
mounted=0
"$build/infilfs-scrub" "$image" | grep -Fq 'Result:              CLEAN'

echo "native user/group/project quota qualification: PASS"
