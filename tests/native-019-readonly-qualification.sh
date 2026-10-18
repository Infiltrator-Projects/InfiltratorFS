#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Qualify 0.19 native RO recovery against disposable loop-backed files only.
# Never operate on a physical partition, despite the user authorising future
# reformatting after the complete 0.19 crash protocol is qualified.
set -Eeuo pipefail

[[ $# == 2 ]] || { echo 'Usage: native-019-readonly-qualification.sh <build> <module>' >&2; exit 2; }
[[ ${EUID} == 0 ]] || { echo 'This test needs root (loop mount).' >&2; exit 2; }
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(realpath "$1")"
module="$(realpath "$2")"
[[ -x "$build/infilfs-format-019-image" && -f "$module" ]] || {
    echo 'Required Format 0.19 image generator or native module missing.' >&2
    exit 2
}

work="$(mktemp -d)"
image="$work/format-019.img"
mnt="$work/mnt"
loop=""
mounted=0
loaded=0
cleanup() {
    local rc=$?
    set +e
    if (( mounted )); then timeout 10s umount "$mnt" || true; fi
    if [[ -n "$loop" ]]; then timeout 10s losetup -d "$loop" || true; fi
    if (( loaded )); then rmmod infiltratorfs || true; fi
    rm -rf -- "$work"
    exit "$rc"
}
trap cleanup EXIT INT TERM
mkdir "$mnt"
"$build/infilfs-format-019-image" --emit-image "$image"
[[ -f "$image" && "$(stat -c %s "$image")" == 16777216 ]] || {
    echo 'Format 0.19 image builder produced invalid length.' >&2; exit 1;
}
if ! grep -q '^infiltratorfs ' /proc/modules; then
    insmod "$module"
    loaded=1
fi
loop="$(losetup --find --show "$image")"
# Verify a fresh superblock cannot be mounted writable. A later RO->RW
# remount has a separate VFS gate checked in check_ro().
if mount -t infiltratorfs -o rw "$loop" "$mnt" 2>/dev/null; then
    mounted=1
    echo 'Unqualified Format 0.19 fresh RW mount succeeded!' >&2
    exit 1
fi
check_ro() {
    mount -t infiltratorfs -o ro "$loop" "$mnt"
    mounted=1
    [[ "$(findmnt -n -o FSTYPE -T "$mnt")" == infiltratorfs ]]
    [[ -z "$(find "$mnt" -mindepth 1 -maxdepth 1 -print -quit)" ]]
    [[ "$(findmnt -n -o OPTIONS -T "$mnt")" == *ro* ]]
    if mount -o remount,rw "$mnt" 2>/dev/null; then
        echo 'Unqualified Format 0.19 writable remount was accepted!' >&2
        exit 1
    fi
    umount "$mnt"
    mounted=0
}
check_ro

# Corrupt block zero; both fixed locators still locate remote checkpoints.
printf '\xff' | dd of="$image" bs=1 seek=0 count=1 conv=notrunc status=none
sync "$image"
blockdev --flushbufs "$loop" || true
check_ro

# One independently corrupted locator must still leave a valid locator.
printf '\xff' | dd of="$image" bs=1 seek=4096 count=1 conv=notrunc status=none
sync "$image"
blockdev --flushbufs "$loop" || true
check_ro

# No valid primary, no valid fixed locator: refuse speculative geometry scan.
printf '\xff' | dd of="$image" bs=1 seek=8384512 count=1 conv=notrunc status=none
sync "$image"
blockdev --flushbufs "$loop" || true
if mount -t infiltratorfs -o ro "$loop" "$mnt" 2>/dev/null; then
    mounted=1
    echo 'Native 0.19 mounted without any valid locator or primary.' >&2
    exit 1
fi
echo 'Format 0.19 loop-backed native read-only recovery: PASS'
