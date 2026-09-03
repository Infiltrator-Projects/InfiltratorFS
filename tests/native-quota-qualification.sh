#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

build="${1:?build directory required}"
module="${2:?kernel module required}"
quota="$build/infiltratorfs-quota"
work="$(mktemp -d)"
image="$work/quota.img"
mnt="$work/mnt"
loopdev=""
mounted=0
loaded=0

cleanup() {
    set +e
    if [[ "$mounted" = 1 ]]; then sudo umount "$mnt" || true; fi
    if [[ -n "$loopdev" ]]; then sudo losetup -d "$loopdev" || true; fi
    if [[ "$loaded" = 1 ]]; then sudo rmmod infiltratorfs || true; fi
    rm -rf "$work"
}
trap cleanup EXIT

field() {
    local key="$1"
    shift
    sudo "$quota" get "$@" | awk -F= -v key="$key" '$1 == key { print $2 }'
}

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
sudo "$quota" set "$mnt" user 0 6MiB 0 >/dev/null
sudo dd if=/dev/urandom of="$mnt/user-bytes.bin" bs=1M count=4 status=none
if sudo dd if=/dev/urandom of="$mnt/user-bytes.bin" bs=1M count=3 \
        oflag=append conv=notrunc status=none 2>"$work/user-edquot"; then
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
user_used="$(field used_objects "$mnt" user 0)"
sudo "$quota" set "$mnt" user 0 0 "$((user_used + 1))" >/dev/null
sudo touch "$mnt/user-object-one"
sudo ln "$mnt/user-object-one" "$mnt/user-object-link"
if sudo touch "$mnt/user-object-two" 2>"$work/user-object-edquot"; then
    echo "user object quota allowed an extra persistent object" >&2
    exit 1
fi
grep -Eqi 'quota|Disk quota exceeded' "$work/user-object-edquot"
test "$(stat -c '%i' "$mnt/user-object-one")" = \
     "$(stat -c '%i' "$mnt/user-object-link")"
sudo "$quota" clear "$mnt" user 0 >/dev/null

# Group object quota uses the same native accounting path.
sudo "$quota" set "$mnt" group 0 0 1000000 >/dev/null
group_used="$(field used_objects "$mnt" group 0)"
sudo "$quota" set "$mnt" group 0 0 "$((group_used + 1))" >/dev/null
sudo touch "$mnt/group-object-one"
if sudo touch "$mnt/group-object-two" 2>"$work/group-edquot"; then
    echo "group object quota allowed an extra persistent object" >&2
    exit 1
fi
grep -Eqi 'quota|Disk quota exceeded' "$work/group-edquot"
sudo "$quota" clear "$mnt" group 0 >/dev/null

# chown transfers accounting. A file larger than the destination user's hard
# limit must stay with its original owner.
sudo dd if=/dev/urandom of="$mnt/chown-candidate.bin" bs=1M count=2 status=none
sudo "$quota" set "$mnt" user 12345 1MiB 0 >/dev/null
if sudo chown 12345:12345 "$mnt/chown-candidate.bin" \
        2>"$work/chown-edquot"; then
    echo "chown crossed a destination user quota" >&2
    exit 1
fi
grep -Eqi 'quota|Disk quota exceeded' "$work/chown-edquot"
test "$(stat -c '%u' "$mnt/chown-candidate.bin")" -eq 0
sudo "$quota" clear "$mnt" user 12345 >/dev/null

# Project IDs are directory-tree quota domains and inherit through descendants.
sudo mkdir "$mnt/project-a" "$mnt/project-b" "$mnt/outside"
sudo "$quota" project-set "$mnt/project-a" 42 >/dev/null
sudo mkdir "$mnt/project-a/sub"
test "$(sudo "$quota" project-get "$mnt/project-a/sub" | \
        awk -F= '$1 == "effective_project_id" { print $2 }')" -eq 42

sudo "$quota" set "$mnt" project 42 5MiB 32 >/dev/null
sudo dd if=/dev/urandom of="$mnt/project-a/data.bin" bs=1M count=4 status=none
if sudo dd if=/dev/urandom of="$mnt/project-a/data.bin" bs=1M count=2 \
        oflag=append conv=notrunc status=none 2>"$work/project42-edquot"; then
    echo "project byte quota allowed an over-limit append" >&2
    exit 1
fi
grep -Eqi 'quota|Disk quota exceeded' "$work/project42-edquot"

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
sudo "$quota" project-set "$mnt/project-b" 43 >/dev/null
sudo "$quota" set "$mnt" project 43 1MiB 32 >/dev/null
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
sudo mkdir "$mnt/outside/project-candidate"
sudo dd if=/dev/urandom of="$mnt/outside/project-candidate/payload.bin" \
    bs=1M count=2 status=none
if sudo "$quota" project-set "$mnt/outside/project-candidate" 43 \
        >"$work/project-set.out" 2>"$work/project-set-edquot"; then
    echo "project assignment crossed the destination project quota" >&2
    exit 1
fi
grep -Eqi 'quota|Disk quota exceeded' "$work/project-set-edquot"
test "$(sudo "$quota" project-get "$mnt/outside/project-candidate" | \
        awk -F= '$1 == "effective_project_id" { print $2 }')" -eq 0

# Deleting or replacing an empty project-root directory must remove the object
# mapping and release the project's object charge.
sudo mkdir "$mnt/project-delete"
sudo "$quota" project-set "$mnt/project-delete" 77 >/dev/null
sudo "$quota" set "$mnt" project 77 0 4 >/dev/null
test "$(field used_objects "$mnt" project 77)" -eq 1
sudo rmdir "$mnt/project-delete"
test "$(field used_objects "$mnt" project 77)" -eq 0

sudo mkdir "$mnt/project-replace" "$mnt/replacement-source"
sudo "$quota" project-set "$mnt/project-replace" 78 >/dev/null
sudo "$quota" set "$mnt" project 78 0 4 >/dev/null
test "$(field used_objects "$mnt" project 78)" -eq 1
sudo mv -T "$mnt/replacement-source" "$mnt/project-replace"
test "$(sudo "$quota" project-get "$mnt/project-replace" | \
        awk -F= '$1 == "effective_project_id" { print $2 }')" -eq 0
test "$(field used_objects "$mnt" project 78)" -eq 0

# Policy and project-root metadata are persistent, but volatile usage is
# deliberately rebuilt from authoritative objects after remount.
before42="$(field used_bytes "$mnt" project 42)"
sudo sync
sudo umount "$mnt"
mounted=0
"$build/infilfs-scrub" "$image" | grep -Fq 'Result:              CLEAN'

sudo mount -t infiltratorfs -o rw "$loopdev" "$mnt"
mounted=1
test "$(sudo "$quota" project-get "$mnt/project-a/sub" | \
        awk -F= '$1 == "effective_project_id" { print $2 }')" -eq 42
after42="$(field used_bytes "$mnt" project 42)"
test "$after42" -eq "$before42"
if sudo dd if=/dev/urandom of="$mnt/project-a/data.bin" bs=1M count=2 \
        oflag=append conv=notrunc status=none 2>"$work/remount-edquot"; then
    echo "persisted project quota was not enforced after remount" >&2
    exit 1
fi
grep -Eqi 'quota|Disk quota exceeded' "$work/remount-edquot"

sudo umount "$mnt"
mounted=0
"$build/infilfs-scrub" "$image" | grep -Fq 'Result:              CLEAN'

echo "native user/group/project quota qualification: PASS"
