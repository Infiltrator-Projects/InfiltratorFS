#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

build="${1:?build directory required}"
module="${2:?kernel module required}"
work="$(mktemp -d)"
image="$work/resize.img"
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

mkdir -p "$mnt"
truncate -s 256M "$image"
"$build/mkfs.infilfs" --force -L NativeResize "$image" >/dev/null

if ! grep -qw infiltratorfs /proc/filesystems; then
    sudo insmod "$module"
    loaded=1
fi
loopdev="$(sudo losetup --find --show "$image")"
sudo mount -t infiltratorfs -o rw "$loopdev" "$mnt"
mounted=1

# Online shrink changes committed filesystem geometry without truncating the backing store.
sudo "$build/infiltratorfs-resize" "$mnt" 128MiB | grep -Fq 'new_size=134217728'
test "$(stat -f -c '%b' "$mnt")" -eq 32768

sudo dd if=/dev/urandom of="$mnt/after-shrink.bin" bs=1M count=8 status=none
sudo sync
small_sha="$(sudo sha256sum "$mnt/after-shrink.bin" | awk '{print $1}')"

# Grow to the physical backing size while still mounted.
sudo "$build/infiltratorfs-resize" "$mnt" max | grep -Fq 'new_size=268435456'
test "$(stat -f -c '%b' "$mnt")" -eq 65536
sudo dd if=/dev/urandom of="$mnt/after-grow.bin" bs=1M count=16 status=none
sudo sync
grow_sha="$(sudo sha256sum "$mnt/after-grow.bin" | awk '{print $1}')"

# Incompressible live data larger than the proposed 64 MiB geometry forces
# allocated blocks into the truncated tail. Shrink must fail closed.
sudo dd if=/dev/urandom of="$mnt/tail-occupier.bin" bs=1M count=96 status=none
sudo sync
if sudo "$build/infiltratorfs-resize" "$mnt" 64MiB >"$work/shrink.out" 2>"$work/shrink.err"; then
    echo 'mounted shrink unexpectedly discarded allocated tail data' >&2
    exit 1
fi
grep -Eqi 'busy|Device or resource busy' "$work/shrink.err"
test "$(stat -f -c '%b' "$mnt")" -eq 65536

sudo umount "$mnt"
mounted=0
"$build/infilfs-scrub" "$image" | grep -Fq 'Result:              CLEAN'

sudo mount -t infiltratorfs -o ro "$loopdev" "$mnt"
mounted=1
test "$(sudo sha256sum "$mnt/after-shrink.bin" | awk '{print $1}')" = "$small_sha"
test "$(sudo sha256sum "$mnt/after-grow.bin" | awk '{print $1}')" = "$grow_sha"
sudo umount "$mnt"
mounted=0

echo 'native mounted resize qualification: PASS'
