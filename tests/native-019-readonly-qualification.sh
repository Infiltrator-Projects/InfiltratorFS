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
[[ -x "$build/infilfs-format-019-image" &&
   -x "$build/infilfs-format-019-posix" && -f "$module" ]] || {
    echo 'Required Format 0.19 image generator, POSIX writer or native module missing.' >&2
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
# Qualify the native reader against a real committed Format 0.19 file graph,
# not merely the empty formatter bootstrap directory. POSIX writer uses
# disposable regular-file images and verifies persistence and deep scrub.
"$build/infilfs-format-019-posix" "$image"
[[ -f "$image" && "$(stat -c %s "$image")" == 16777216 ]] || {
    echo 'Format 0.19 image builder produced invalid length.' >&2; exit 1;
}
if ! grep -q '^infiltratorfs ' /proc/modules; then
    insmod "$module"
    loaded=1
fi
loop="$(losetup --find --show "$image")"
# Do not use /bin/mount to assert rejection of a requested RW mount:
# util-linux may fall back transparently to RO after an EROFS kernel error.
# Call the kernel directly, with no implicit retry or fallback.
python3 - "$loop" "$mnt" <<'PY'
import ctypes
import errno
import os
import subprocess
import sys

libc = ctypes.CDLL(None, use_errno=True)
libc.mount.argtypes = [
    ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
    ctypes.c_ulong, ctypes.c_void_p
]
libc.mount.restype = ctypes.c_int
target = os.fsencode(sys.argv[2])
ret = libc.mount(os.fsencode(sys.argv[1]), target, b"infiltratorfs", 0, None)
if ret == 0:
    subprocess.run(["umount", sys.argv[2]], check=True)
    raise SystemExit("Unsafe: native 0.19 accepted a raw RW mount syscall")
err = ctypes.get_errno()
if err != errno.EROFS:
    raise SystemExit(f"Unexpected native 0.19 RW error: {os.strerror(err)}")
PY
check_ro() {
    mount -t infiltratorfs -o ro "$loop" "$mnt"
    mounted=1
    [[ "$(findmnt -n -o FSTYPE -T "$mnt")" == infiltratorfs ]]
    [[ -f "$mnt/persistent" ]]
    [[ "$(findmnt -n -o OPTIONS -T "$mnt")" == *ro* ]]
    # Bit-for-bit portable/native parity across fresh, recovered and
    # locator-damaged mounts. This catches namespace and data-path regressions.
    python3 - "$mnt/persistent" <<'PYVERIFY'
import pathlib
import sys
path = pathlib.Path(sys.argv[1])
expected = bytes((i * 109 + (i >> 9)) & 255 for i in range(256 * 1024))
actual = path.read_bytes()
if actual != expected:
    raise SystemExit(f"Native 0.19 data mismatch: {len(actual)} bytes")
PYVERIFY
    python3 - "$mnt" <<'PY'
import ctypes
import errno
import os
import sys

libc = ctypes.CDLL(None, use_errno=True)
libc.mount.argtypes = [
    ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
    ctypes.c_ulong, ctypes.c_void_p
]
libc.mount.restype = ctypes.c_int
ret = libc.mount(None, os.fsencode(sys.argv[1]), None, 32, None)
if ret == 0 and not (os.statvfs(sys.argv[1]).f_flag & os.ST_RDONLY):
    raise SystemExit("Unsafe: native 0.19 accepted a writable remount")
if ret != 0 and ctypes.get_errno() != errno.EROFS:
    err = ctypes.get_errno()
    raise SystemExit(f"Unexpected native 0.19 remount error: {os.strerror(err)}")
PY
    umount "$mnt"
    mounted=0
}
check_ro

# Both fixed locators can be lost while block zero remains an intact,
# independently validated checkpoint. Restore the disposable image's locator
# pages before the subsequent block-zero-loss recovery matrix.
dd if="$image" of="$work/locator-a.saved" bs=4096 skip=1 count=1 status=none
dd if="$image" of="$work/locator-b.saved" bs=4096 skip=2047 count=1 status=none
printf '\xff' | dd of="$image" bs=1 seek=4096 count=1 conv=notrunc status=none
printf '\xff' | dd of="$image" bs=1 seek=8384512 count=1 conv=notrunc status=none
sync "$image"
blockdev --flushbufs "$loop" || true
check_ro
dd if="$work/locator-a.saved" of="$image" bs=4096 seek=1 count=1 conv=notrunc status=none
dd if="$work/locator-b.saved" of="$image" bs=4096 seek=2047 count=1 conv=notrunc status=none
sync "$image"
blockdev --flushbufs "$loop" || true
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
