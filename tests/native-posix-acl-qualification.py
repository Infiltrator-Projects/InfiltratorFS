#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Mounted native POSIX ACL qualification for InfiltratorFS.

The test deliberately uses the Linux POSIX ACL xattr ABI directly rather than
setfacl/getfacl so the native kernel gate does not depend on the acl userspace
package.  rsync -aA is still exercised because it is the migration path used by
Linux root-filesystem qualification.
"""

import os
import pwd
import shutil
import struct
import subprocess
import sys

POSIX_ACL_XATTR_VERSION = 0x0002
ACL_USER_OBJ = 0x01
ACL_USER = 0x02
ACL_GROUP_OBJ = 0x04
ACL_MASK = 0x10
ACL_OTHER = 0x20
ACL_UNDEFINED_ID = 0xFFFFFFFF
ACL_READ = 0x04
ACL_WRITE = 0x02
ACL_EXECUTE = 0x01
ACCESS_NAME = b"system.posix_acl_access"
DEFAULT_NAME = b"system.posix_acl_default"


def fail(message):
    raise AssertionError(message)


def nobody_ids():
    try:
        entry = pwd.getpwnam("nobody")
        return entry.pw_uid, entry.pw_gid
    except KeyError:
        return 65534, 65534


def encode_acl(entries):
    blob = bytearray(struct.pack("<I", POSIX_ACL_XATTR_VERSION))
    for tag, perm, ident in entries:
        blob += struct.pack("<HHI", tag, perm, ident)
    return bytes(blob)


def decode_acl(blob):
    if len(blob) < 4 or (len(blob) - 4) % 8:
        fail(f"malformed ACL xattr length {len(blob)}")
    (version,) = struct.unpack_from("<I", blob, 0)
    if version != POSIX_ACL_XATTR_VERSION:
        fail(f"unexpected ACL xattr version {version}")
    entries = []
    for offset in range(4, len(blob), 8):
        entries.append(struct.unpack_from("<HHI", blob, offset))
    return entries


def find_entry(entries, tag, ident=None):
    matches = []
    for entry_tag, perm, entry_id in entries:
        if entry_tag != tag:
            continue
        if ident is not None and entry_id != ident:
            continue
        matches.append((perm, entry_id))
    if len(matches) != 1:
        fail(f"expected one ACL entry tag={tag} id={ident}, got {matches}")
    return matches[0]


def assert_named_read_acl(path, uid, expected_mask=ACL_READ):
    entries = decode_acl(os.getxattr(path, ACCESS_NAME))
    named_perm, _ = find_entry(entries, ACL_USER, uid)
    mask_perm, _ = find_entry(entries, ACL_MASK)
    other_perm, _ = find_entry(entries, ACL_OTHER)
    if named_perm != ACL_READ:
        fail(f"named user ACL changed on {path}: {named_perm}")
    if mask_perm != expected_mask:
        fail(f"ACL mask mismatch on {path}: {mask_perm} != {expected_mask}")
    if other_perm != 0:
        fail(f"other permissions unexpectedly grant access on {path}: {other_perm}")


def can_read_as(path, uid, gid):
    pid = os.fork()
    if pid == 0:
        try:
            os.setgroups([])
            os.setgid(gid)
            os.setuid(uid)
            fd = os.open(path, os.O_RDONLY)
            try:
                os.read(fd, 1)
            finally:
                os.close(fd)
        except BaseException:
            os._exit(1)
        os._exit(0)
    _, status = os.waitpid(pid, 0)
    return os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0


def access_acl(uid):
    return encode_acl(
        [
            (ACL_USER_OBJ, ACL_READ | ACL_WRITE, ACL_UNDEFINED_ID),
            (ACL_USER, ACL_READ, uid),
            (ACL_GROUP_OBJ, 0, ACL_UNDEFINED_ID),
            (ACL_MASK, ACL_READ, ACL_UNDEFINED_ID),
            (ACL_OTHER, 0, ACL_UNDEFINED_ID),
        ]
    )


def default_acl(uid):
    return encode_acl(
        [
            (ACL_USER_OBJ, ACL_READ | ACL_WRITE | ACL_EXECUTE, ACL_UNDEFINED_ID),
            (ACL_USER, ACL_READ, uid),
            (ACL_GROUP_OBJ, 0, ACL_UNDEFINED_ID),
            (ACL_MASK, ACL_READ, ACL_UNDEFINED_ID),
            (ACL_OTHER, 0, ACL_UNDEFINED_ID),
        ]
    )


def create_inherited_file(directory, name, uid, gid):
    path = os.path.join(directory, name)
    old_umask = os.umask(0o077)
    try:
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o666)
    finally:
        os.umask(old_umask)
    try:
        os.write(fd, b"inherited-acl\n")
        os.fsync(fd)
    finally:
        os.close(fd)
    assert_named_read_acl(path, uid)
    if not can_read_as(path, uid, gid):
        fail("default ACL did not grant inherited named-user read access")
    return path


def prepare(root):
    uid, gid = nobody_ids()
    acl_root = os.path.join(root, "posix-acl-qualification")
    os.mkdir(acl_root, 0o755)
    os.chmod(acl_root, 0o755)

    access = os.path.join(acl_root, "access.txt")
    fd = os.open(access, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    try:
        os.write(fd, b"named-user-access\n")
        os.fsync(fd)
    finally:
        os.close(fd)
    if can_read_as(access, uid, gid):
        fail("mode 0600 unexpectedly allowed nobody before ACL")

    os.setxattr(access, ACCESS_NAME, access_acl(uid))
    assert_named_read_acl(access, uid)
    if not can_read_as(access, uid, gid):
        fail("access ACL did not grant named-user read access")

    os.chmod(access, 0o600)
    assert_named_read_acl(access, uid, expected_mask=0)
    if can_read_as(access, uid, gid):
        fail("chmod 0600 did not restrict the POSIX ACL mask")

    os.chmod(access, 0o640)
    assert_named_read_acl(access, uid, expected_mask=ACL_READ)
    if not can_read_as(access, uid, gid):
        fail("chmod 0640 did not restore named-user effective read access")

    inherit = os.path.join(acl_root, "inherit")
    os.mkdir(inherit, 0o755)
    os.chmod(inherit, 0o755)
    os.setxattr(inherit, DEFAULT_NAME, default_acl(uid))
    if os.getxattr(inherit, DEFAULT_NAME) != default_acl(uid):
        fail("default ACL readback mismatch")
    create_inherited_file(inherit, "before-remount.txt", uid, gid)

    rsync = shutil.which("rsync")
    if not rsync:
        fail("rsync is required for root-migration ACL qualification")
    src = os.path.join(acl_root, "rsync-src")
    dst = os.path.join(acl_root, "rsync-dst")
    os.mkdir(src, 0o755)
    os.mkdir(dst, 0o755)
    source = os.path.join(src, "file.txt")
    fd = os.open(source, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    try:
        os.write(fd, b"rsync-acl\n")
        os.fsync(fd)
    finally:
        os.close(fd)
    os.setxattr(source, ACCESS_NAME, access_acl(uid))
    subprocess.run([rsync, "-aA", src + "/", dst + "/"], check=True)
    copied = os.path.join(dst, "file.txt")
    if open(copied, "rb").read() != b"rsync-acl\n":
        fail("rsync data mismatch")
    if os.getxattr(source, ACCESS_NAME) != os.getxattr(copied, ACCESS_NAME):
        fail("rsync -aA did not preserve the POSIX ACL")
    if not can_read_as(copied, uid, gid):
        fail("rsync-preserved ACL is not enforced")

    dfd = os.open(acl_root, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(dfd)
    finally:
        os.close(dfd)
    print("native POSIX ACL qualification: PREPARE PASS")


def verify(root):
    uid, gid = nobody_ids()
    acl_root = os.path.join(root, "posix-acl-qualification")
    access = os.path.join(acl_root, "access.txt")
    assert_named_read_acl(access, uid)
    if not can_read_as(access, uid, gid):
        fail("access ACL was not enforced after remount")

    inherit = os.path.join(acl_root, "inherit")
    if os.getxattr(inherit, DEFAULT_NAME) != default_acl(uid):
        fail("default ACL did not persist across remount")
    assert_named_read_acl(os.path.join(inherit, "before-remount.txt"), uid)
    create_inherited_file(inherit, "after-remount.txt", uid, gid)

    source = os.path.join(acl_root, "rsync-src", "file.txt")
    copied = os.path.join(acl_root, "rsync-dst", "file.txt")
    if os.getxattr(source, ACCESS_NAME) != os.getxattr(copied, ACCESS_NAME):
        fail("rsync ACL copies diverged after remount")
    if not can_read_as(copied, uid, gid):
        fail("rsync-preserved ACL was not enforced after remount")

    print("native POSIX ACL qualification: REMOUNT PASS")


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in {"prepare", "verify"}:
        raise SystemExit(
            "usage: native-posix-acl-qualification.py prepare|verify MOUNTPOINT"
        )
    root = os.path.abspath(sys.argv[2])
    if os.geteuid() != 0:
        raise SystemExit("native POSIX ACL qualification must run as root")
    if sys.argv[1] == "prepare":
        prepare(root)
    else:
        verify(root)


if __name__ == "__main__":
    main()
