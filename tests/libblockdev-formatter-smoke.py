#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import os
import subprocess
import sys
import tempfile

import gi

gi.require_version("BlockDev", "3.0")
from gi.repository import BlockDev


def fail(message: str) -> None:
    raise SystemExit(f"libblockdev-formatter-smoke: {message}")


plugins = BlockDev.plugin_specs_from_names(("fs",))
if not BlockDev.is_initialized():
    if not BlockDev.init(plugins, None):
        fail("could not initialize the filesystem plugin")
else:
    if not BlockDev.reinit(plugins, True, None):
        fail("could not reinitialize the filesystem plugin")

if not BlockDev.fs_is_tech_avail(
    BlockDev.FSTech.INFILTRATORFS, BlockDev.FSTechMode.MKFS
):
    fail("InfiltratorFS mkfs technology is unavailable")

available, flags, utility = BlockDev.fs_can_mkfs("infiltratorfs")
if not available or utility is not None:
    fail(f"CanFormat failed: available={available}, utility={utility!r}")
if not (flags & BlockDev.FSMkfsOptionsFlags.LABEL):
    fail("label creation feature is missing")
if not (flags & BlockDev.FSMkfsOptionsFlags.FORCE):
    fail("force creation feature is missing")

with tempfile.TemporaryDirectory(prefix="infiltratorfs-libblockdev-") as tmp:
    image = os.path.join(tmp, "formatter.img")
    with open(image, "wb") as stream:
        stream.truncate(64 * 1024 * 1024)
    options = BlockDev.FSMkfsOptions(label="GNOME Disks Integration")
    if not BlockDev.fs_mkfs(image, "infiltratorfs", options):
        fail("generic fs_mkfs returned false")
    probe = subprocess.check_output(
        ["infilfs-inspect", "--udev", image], text=True
    )
    expected = {
        "ID_FS_USAGE=filesystem",
        "ID_FS_TYPE=infiltratorfs",
        "ID_FS_VERSION=0.17",
        "ID_FS_LABEL=GNOME Disks Integration",
    }
    missing = sorted(expected.difference(probe.splitlines()))
    if missing:
        fail(f"formatted image probe is missing {missing}")

    # UDisks performs bd_fs_clean() before every format and waits for the old
    # filesystem identity to disappear.  This used to be missed because
    # libblkid does not yet know InfiltratorFS, causing GNOME Disks to time out
    # after its "initial wipe" on an already formatted volume.
    if not BlockDev.fs_clean(image):
        fail("generic fs_clean returned false for an InfiltratorFS image")
    cleaned = subprocess.run(
        ["infilfs-inspect", "--udev", image],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if cleaned.returncode == 0 or cleaned.stdout:
        fail("fs_clean left the InfiltratorFS signature visible")

    if not BlockDev.fs_mkfs(image, "infiltratorfs", options):
        fail("reformat after fs_clean returned false")
    reprobe = subprocess.check_output(
        ["infilfs-inspect", "--udev", image], text=True
    )
    missing = sorted(expected.difference(reprobe.splitlines()))
    if missing:
        fail(f"reformatted image probe is missing {missing}")

print("libblockdev-formatter-smoke: PASS")
