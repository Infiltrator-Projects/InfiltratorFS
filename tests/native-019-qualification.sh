#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Native Format 0.19 qualification on disposable loop-backed files only.
set -Eeuo pipefail
[[ $# == 2 ]] || { echo 'Usage: native-019-qualification.sh <build> <module>' >&2; exit 2; }
[[ ${EUID} == 0 ]] || { echo 'This test needs root (loop mount).' >&2; exit 2; }
build="$(realpath "$1")"
module="$(realpath "$2")"
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
truncate -s 16M "$image"
"$build/mkfs.infilfs" --force -L Native19 "$image" | grep -Fq 'Format:          0.19'
"$build/infilfs-format-019-posix" "$image"
cp "$image" "$work/original.img"
dd if="$image" of="$work/old-locators-a" bs=4096 skip=1 count=1 status=none
dd if="$image" of="$work/old-locators-b" bs=4096 skip=2047 count=1 status=none
if ! grep -q '^infiltratorfs ' /proc/modules; then
    insmod "$module"
    loaded=1
fi
truncate -s 64M "$image"
loop="$(losetup --find --show "$image")"
verify() {
    python3 - "$mnt/persistent" <<'PYVERIFY'
from pathlib import Path
import sys
actual = Path(sys.argv[1]).read_bytes()
expected = bytes((i * 109 + (i >> 9)) & 255 for i in range(256 * 1024))
assert actual == expected, 'portable/native data mismatch'
PYVERIFY
}
check_offline() {
    "$build/fsck.infiltratorfs" -n "$image" | grep -Fq 'Result:                CLEAN'
    "$build/fsck.infiltratorfs" --scrub -n "$image" >"$work/scrub.out" 2>"$work/scrub.err" || {
        cat "$work/scrub.out" "$work/scrub.err" >&2
        return 1
    }
    grep -Fq 'Result:              CLEAN' "$work/scrub.out"
}
mount -t infiltratorfs -o ro "$loop" "$mnt"
mounted=1
verify
mount -o remount,rw "$mnt"
[[ "$(findmnt -n -o OPTIONS -T "$mnt")" != *ro,* ]]
printf 'Native Format 0.19 durable write\n' > "$mnt/native-file"
mkdir "$mnt/directory"
ln "$mnt/native-file" "$mnt/directory/hardlink"
ln -s ../native-file "$mnt/directory/symlink"
cmp "$mnt/native-file" "$mnt/directory/hardlink"
cmp "$mnt/native-file" "$mnt/directory/symlink"
sync -f "$mnt"
# Relocate checkpoints in both directions while preserving committed data.
"$build/infiltratorfs-resize" "$mnt" 32MiB | grep -Fq 'new_size=33554432'
"$build/infiltratorfs-resize" "$mnt" 16MiB | grep -Fq 'new_size=16777216'
"$build/infiltratorfs-resize" "$mnt" max | grep -Fq 'new_size=67108864'
verify
umount "$mnt"
mounted=0
check_offline
# Both old locator hints must not hide a newer valid block-zero geometry.
dd if="$work/old-locators-a" of="$image" bs=4096 seek=1 count=1 conv=notrunc status=none
dd if="$work/old-locators-b" of="$image" bs=4096 seek=2047 count=1 conv=notrunc status=none
sync "$image"
blockdev --flushbufs "$loop"
mount -t infiltratorfs -o ro "$loop" "$mnt"
mounted=1
[[ "$(stat -f -c %b "$mnt")" == 16384 ]]
verify
mount -o remount,rw "$mnt"
"$build/infiltratorfs-resize" "$mnt" 16MiB | grep -Fq 'new_size=16777216'
umount "$mnt"
mounted=0
losetup -d "$loop"
loop=""
# A completed shrink must survive actual reduction of backing capacity.
truncate -s 16M "$image"
check_offline
cp "$image" "$work/healthy.img"
loop="$(losetup --find --show "$image")"
for damage in 0 1 2047 '1 2047' '0 1' '0 2047' '0 1 2047'; do
    blockdev --flushbufs "$loop"
    cp "$work/healthy.img" "$image"
    for block in $damage; do
        printf '\xff' | dd of="$image" bs=1 seek="$((block * 4096))" count=1 conv=notrunc status=none
    done
    sync "$image"
    blockdev --flushbufs "$loop"
    before="$(sha256sum "$image" | cut -d' ' -f1)"
    if [[ "$damage" == '0 1 2047' ]]; then
        if mount -t infiltratorfs -o ro "$loop" "$mnt" 2>/dev/null; then
            mounted=1
            echo 'Mounted without any independently valid discovery map' >&2
            exit 1
        fi
        [[ "$(sha256sum "$image" | cut -d' ' -f1)" == "$before" ]]
        continue
    fi
    mount -t infiltratorfs -o ro "$loop" "$mnt"
    mounted=1
    verify
    cmp "$mnt/native-file" "$mnt/directory/hardlink"
    umount "$mnt"
    mounted=0
    [[ "$(sha256sum "$image" | cut -d' ' -f1)" == "$before" ]]
    mount -t infiltratorfs -o rw "$loop" "$mnt"
    mounted=1
    verify
    printf 'recovered\n' > "$mnt/after-recovery"
    sync -f "$mnt"
    umount "$mnt"
    mounted=0
    blockdev --flushbufs "$loop"
    check_offline
    printf 'Native 0.19 recovery and writable repair after damage [%s]: PASS\n' "$damage"
done
# Feature admission must be decided before repair or writer initialization.
# This edits CRC-valid checkpoints, retaining the same locator geometry.
patch_checkpoints() {
    python3 - "$image" "$1" <<'PYPATCH'
from pathlib import Path
import struct
import sys
p = Path(sys.argv[1])
data = bytearray(p.read_bytes())
header_bytes = struct.unpack_from('<H', data, 12)[0]
positions = struct.unpack_from('<QQQ', data, 76)
def crc64(block):
    crc = 0
    for byte in block:
        crc ^= byte << 56
        for _ in range(8):
            crc = ((crc << 1) ^ (0x42f0e1eba9ea3693 if crc >> 63 else 0)) & ((1 << 64) - 1)
    return crc
for pos in positions:
    offset = pos * 4096
    block = data[offset:offset + 4096]
    if sys.argv[2] == 'ro-feature':
        struct.pack_into('<Q', block, 132, 1 << 63)  # compatible: ignored
        struct.pack_into('<Q', block, 140, 1 << 63)  # RO-compatible: no writes
    else:
        struct.pack_into('<H', block, 10, 18)
    checksum = header_bytes - 32
    block[checksum:checksum + 32] = bytes(32)
    struct.pack_into('<Q', block, checksum, crc64(block))
    data[offset:offset + 4096] = block
if sys.argv[2] == 'old-format':
    for pos in (1, 2047):
        data[pos * 4096:(pos + 1) * 4096] = bytes(4096)
p.write_bytes(data)
PYPATCH
    sync "$image"
    blockdev --flushbufs "$loop"
}
refuse_rw() {
    python3 - "$loop" "$mnt" "$1" <<'PYREFUSE'
import ctypes
import errno
import sys
libc = ctypes.CDLL(None, use_errno=True)
libc.mount.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
                       ctypes.c_ulong, ctypes.c_void_p]
rc = libc.mount(sys.argv[1].encode(), sys.argv[2].encode(), b'infiltratorfs',
                int(sys.argv[3]), None)
assert rc == -1 and ctypes.get_errno() == errno.EROFS, 'unknown feature admitted writable mount/remount'
PYREFUSE
}
blockdev --flushbufs "$loop"
cp "$work/healthy.img" "$image"
patch_checkpoints ro-feature
before="$(sha256sum "$image" | cut -d' ' -f1)"
refuse_rw 0
mount -t infiltratorfs -o ro "$loop" "$mnt"
mounted=1
verify
refuse_rw 32
umount "$mnt"
mounted=0
[[ "$(sha256sum "$image" | cut -d' ' -f1)" == "$before" ]]
cp "$work/healthy.img" "$image"
patch_checkpoints old-format
before="$(sha256sum "$image" | cut -d' ' -f1)"
if mount -t infiltratorfs -o ro "$loop" "$mnt" 2>/dev/null; then
    mounted=1
    echo 'Mounted superseded Format 0.18' >&2
    exit 1
fi
[[ "$(sha256sum "$image" | cut -d' ' -f1)" == "$before" ]]
echo 'Format 0.19 feature admission and old-format rejection: PASS'
echo 'Format 0.19 native write/remount/grow/shrink/recovery/repair: PASS'
