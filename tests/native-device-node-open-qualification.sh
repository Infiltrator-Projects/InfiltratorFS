#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-build-root-boot}"
module="${2:-kernel/infiltratorfs.ko}"
work="${RUNNER_TEMP:-/tmp}/infiltratorfs-device-node-open"
image="$work/device-node.raw"
mnt="$work/mnt"
loop=""

cleanup() {
    set +e
    mountpoint -q "$mnt" && umount -l "$mnt"
    [[ -n "$loop" ]] && losetup -d "$loop" 2>/dev/null
    rmmod infiltratorfs 2>/dev/null
}
trap cleanup EXIT

[[ $EUID -eq 0 ]] || { echo 'native device-node qualification requires root' >&2; exit 2; }
for cmd in findmnt losetup mknod mount mountpoint stat umount; do
    command -v "$cmd" >/dev/null
 done

rm -rf "$work"
mkdir -p "$work" "$mnt"
truncate -s 256M "$image"
"$build/mkfs.infilfs" -L DeviceNodeOpen "$image" >/dev/null
loop="$(losetup --find --show "$image")"
insmod "$module"
mount -t infiltratorfs -o rw,dev,exec "$loop" "$mnt"

echo "Mounted options: $(findmnt -n -o OPTIONS --target "$mnt")"
node="$mnt/test-dev-null"
mknod "$node" c 1 3
stat -c 'Created node: type=%F mode=%f rdev-major=%t rdev-minor=%T inode=%i' "$node"
[[ -c "$node" ]]
printf 'device-write-probe\n' >"$node"

umount "$mnt"
mount -t infiltratorfs -o rw,dev,exec "$loop" "$mnt"
node="$mnt/test-dev-null"
stat -c 'Remounted node: type=%F mode=%f rdev-major=%t rdev-minor=%T inode=%i' "$node"
[[ -c "$node" ]]
printf 'device-remount-probe\n' >"$node"
rm -f "$node"
sync

echo 'Native character-device create/open/remount qualification: PASS'
