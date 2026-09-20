#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

build="${1:-build}"
module="${2:-kernel/infiltratorfs.ko}"
image="$(mktemp --suffix=.img)"
mountpoint="$(mktemp -d)"
loopdev=""
mounted=0
loaded=0

cleanup() {
    set +e
    if [[ "$mounted" = 1 ]]; then sudo umount "$mountpoint"; fi
    if [[ -n "$loopdev" ]]; then sudo losetup -d "$loopdev"; fi
    if [[ "$loaded" = 1 ]]; then sudo rmmod infiltratorfs; fi
    rm -f "$image"
    rmdir "$mountpoint" 2>/dev/null || true
}
trap cleanup EXIT

truncate -s 64M "$image"
"$build/mkfs.infilfs" --removable-profile -L removable "$image"
loopdev="$(sudo losetup --find --show "$image")"

if ! lsmod | grep -q '^infiltratorfs '; then
    sudo insmod "$module"
    loaded=1
fi
sudo mount -t infiltratorfs -o rw "$loopdev" "$mountpoint"
mounted=1

sudo touch "$mountpoint/resume-é.txt"
sudo touch "$mountpoint/com10.txt"
for name in CON con.txt PRN.doc NUL COM1.log LPT9 'bad:name' 'bad?name' 'bad*name' 'bad|name' 'bad<name' 'bad>name' 'bad"name' 'trail.' 'trail '; do
    if sudo touch "$mountpoint/$name" 2>/dev/null; then
        echo "native removable profile accepted forbidden name: $name" >&2
        exit 1
    fi
done
sudo sync
sudo umount "$mountpoint"
mounted=0
sudo losetup -d "$loopdev"
loopdev=""

"$build/fsck.infiltratorfs" --scrub "$image" | tee /tmp/infilfs-removable-scrub.txt
grep -Fq 'Result:              CLEAN' /tmp/infilfs-removable-scrub.txt
"$build/infilfs-tool" "$image" ls / | grep -Fq 'resume-é.txt'
"$build/infilfs-tool" "$image" ls / | grep -Fq 'com10.txt'
printf 'Native removable-volume filename profile qualification passed.\n'
