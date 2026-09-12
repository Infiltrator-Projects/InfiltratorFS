#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail

build="${1:?build directory required}"
module="${2:?kernel module required}"
mkfs="$build/mkfs.infilfs"
fsck="$build/fsck.infiltratorfs"
max_xattr_seconds="${INFILFS_XATTR_MAX_SECONDS:-60}"
work="$(mktemp -d)"
image="$work/linux-meta.img"
mnt="$work/mnt"
loopdev=""
mounted=0
loaded=0

on_error() {
    local rc=$?
    printf 'native Linux metadata performance qualification: FAIL line=%s rc=%d command=%s\n' \
        "${BASH_LINENO[0]:-$LINENO}" "$rc" "$BASH_COMMAND" >&2
    return "$rc"
}
trap on_error ERR

cleanup() {
    set +e
    sync
    if [[ "$mounted" = 1 ]]; then sudo umount "$mnt" || true; fi
    if [[ -n "$loopdev" ]]; then sudo losetup -d "$loopdev" || true; fi
    if [[ "$loaded" = 1 ]]; then sudo rmmod infiltratorfs || true; fi
    rm -rf "$work"
}
trap cleanup EXIT

stage() {
    printf 'native Linux metadata performance qualification: STAGE %s\n' "$1" >&2
}

for tool in "$mkfs" "$fsck" "$module"; do
    test -s "$tool"
done

stage "format and mount"
mkdir -p "$mnt"
truncate -s 4G "$image"
"$mkfs" --force -L NativeLinuxMetadata "$image" >/dev/null

if grep -qw infiltratorfs /proc/filesystems; then
    sudo rmmod infiltratorfs
fi
sudo insmod "$module"
loaded=1
loopdev="$(sudo losetup --find --show "$image")"
sudo mount -t infiltratorfs -o rw "$loopdev" "$mnt"
mounted=1

stage "create 5000 files and set ordinary Linux xattrs"
sudo python3 - "$mnt" "$max_xattr_seconds" <<'PY'
import os
import sys
import time

root = sys.argv[1]
limit = float(sys.argv[2])
count = 5000
paths = []
payload = b'x' * 4096

start = time.monotonic()
for i in range(count):
    path = os.path.join(root, f'f{i:05d}')
    with open(path, 'wb', buffering=0) as handle:
        handle.write(payload)
    paths.append(path)
create_elapsed = time.monotonic() - start

start = time.monotonic()
for path in paths:
    os.setxattr(path, b'user.infiltratorfs-ci', b'1')
xattr_elapsed = time.monotonic() - start

print(f'5000 create+4K: {create_elapsed:.3f}s')
print(f'5000 xattr sets: {xattr_elapsed:.3f}s')
if xattr_elapsed > limit:
    raise SystemExit(
        f'Linux xattr path exceeded qualification bound: '
        f'{xattr_elapsed:.3f}s > {limit:.3f}s')

for path in paths[::113]:
    assert os.getxattr(path, b'user.infiltratorfs-ci') == b'1'
PY

stage "offline scrub after first publication"
sync
sudo umount "$mnt"
mounted=0
sudo "$fsck" --scrub "$loopdev" | tee "$work/scrub-before-remount.txt"
grep -Fq 'Result:              CLEAN' "$work/scrub-before-remount.txt"

stage "remount and verify persisted xattrs"
sudo mount -t infiltratorfs -o rw "$loopdev" "$mnt"
mounted=1
sudo python3 - "$mnt" <<'PY'
import os
import sys

root = sys.argv[1]
for i in range(0, 5000, 113):
    path = os.path.join(root, f'f{i:05d}')
    assert os.getxattr(path, b'user.infiltratorfs-ci') == b'1'
print('xattr remount persistence: PASS')
PY

stage "final unmount and offline scrub"
sync
sudo umount "$mnt"
mounted=0
sudo "$fsck" --scrub "$loopdev" | tee "$work/scrub-final.txt"
grep -Fq 'Result:              CLEAN' "$work/scrub-final.txt"

sudo losetup -d "$loopdev"
loopdev=""
sudo rmmod infiltratorfs
loaded=0
trap - EXIT
rm -rf "$work"
printf 'native Linux metadata performance qualification: PASS\n'
