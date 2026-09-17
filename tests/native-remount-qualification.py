#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise live VFS remounts, without the mount.infiltratorfs helper."""
import ctypes
import errno
import os
from pathlib import Path
import sys

root = Path(sys.argv[1])
libc = ctypes.CDLL(None, use_errno=True)
libc.mount.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
                       ctypes.c_ulong, ctypes.c_char_p]
libc.mount.restype = ctypes.c_int
MS_RDONLY, MS_REMOUNT = 1, 32


def remount(readonly, options=None, expected=None):
    flags = MS_REMOUNT | (MS_RDONLY if readonly else 0)
    result = libc.mount(None, os.fsencode(root), None, flags, options)
    error = ctypes.get_errno() if result else 0
    assert error == (expected or 0), ("remount", readonly, error, expected)


def expect_error(code, operation):
    try:
        operation()
    except OSError as exc:
        assert exc.errno == code, exc
    else:
        raise AssertionError(f"expected errno {code}")


def create_probe():
    fd = os.open(root / "ro-probe", os.O_WRONLY | os.O_CREAT, 0o600)
    os.close(fd)


def check_options(readonly):
    rows = [line.split() for line in Path("/proc/mounts").read_text().splitlines()]
    opts = next(row[3].split(",") for row in rows if row[1] == str(root))
    assert ("ro" if readonly else "rw") in opts, opts
    assert "compress=off" in opts and "media=balanced" in opts, opts


check_options(True)
expect_error(errno.EROFS, create_probe)
# A failed option change must not enable writes or lose the live policy.
remount(False, b"compress=auto", errno.EINVAL)
check_options(True)
expect_error(errno.EROFS, create_probe)
remount(False)
check_options(False)

if len(sys.argv) > 2 and sys.argv[2] == "quota":
    with open(root / "over-quota", "wb", buffering=0) as stream:
        expect_error(errno.EDQUOT, lambda: stream.write(b"q" * (40 * 1024 * 1024)))
    (root / "over-quota").unlink()
    print("Persisted quota enforced after initial RO-to-RW promotion: PASS")
    sys.exit(0)

work = root / "remount-work"
work.mkdir()
os.chmod(work, 0o750)
os.chown(work, 0, 0)
os.setxattr(work, b"user.remount", b"metadata")
assert os.getxattr(work, b"user.remount") == b"metadata"
assert work.stat().st_mode & 0o777 == 0o750
payload = bytes(range(256)) * (4 * 1024 * 1024 // 256)
# Deliberately no fsync: remount RO must drain dirty page-cache writes itself.
(work / "buffered").write_bytes(payload)
(work / "buffered").rename(work / "durable")
(root / "snapshot-live.txt").write_bytes(b"after-remount\n")

with open(work / "busy", "wb"):
    remount(True, expected=errno.EBUSY)
check_options(False)
(work / "busy").unlink()

# VFS refuses a normal RO remount while an unlinked inode is still open,
# even if that descriptor is read-only. Refusal must leave it readable.
orphan = work / "open-unlinked"
orphan.write_bytes(b"keep-open" * 8192)
with open(orphan, "rb", buffering=0) as stream:
    orphan.unlink()
    remount(True, expected=errno.EBUSY)
    assert stream.read() == b"keep-open" * 8192

# Linked read-open files are allowed across the live mode transitions.
with open(work / "durable", "rb", buffering=0) as stream:
    for _ in range(3):
        remount(True)
        check_options(True)
        expect_error(errno.EROFS, create_probe)
        assert (work / "durable").read_bytes() == payload
        remount(False, b"compress=off,media=balanced")
        check_options(False)
        stream.seek(0)
        assert stream.read() == payload
        (work / "temporary").write_bytes(b"namespace write works")
        (work / "temporary").unlink()

remount(True)
expect_error(errno.EROFS, create_probe)
remount(False)
assert (work / "durable").read_bytes() == payload
print("Initial RO promotion, repeated live remounts, metadata, buffered writes, "
      "failed remounts and open-unlinked lifetime: PASS")
