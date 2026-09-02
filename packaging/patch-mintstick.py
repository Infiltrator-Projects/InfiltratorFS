#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Patch Linux Mint's stock Mintstick formatter for InfiltratorFS.

The installed integration helper feeds this program the diverted stock files,
never an already-patched copy. Exact, counted replacements make an incompatible
future Mintstick change fail visibly instead of silently damaging the formatter.
"""

from __future__ import annotations

import argparse
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(
            f"mintstick integration: expected exactly one {label} anchor, found {count}"
        )
    return text.replace(old, new, 1)


def patch_ui(text: str) -> str:
    if '"infiltratorfs"' not in text:
        old = '            self.fsmodel.append(["ext4", "EXT4", 16, False, False])'
        new = (
            old
            + '\n            self.fsmodel.append('
            + '["infiltratorfs", "InfiltratorFS", 63, False, False])'
        )
        text = replace_once(text, old, new, "filesystem-list")

    # Mintstick historically removed every digit from the selected block path.
    # That happens to turn /dev/sdb1 into /dev/sdb, but it corrupts Linux
    # digit-ending whole-disk names: /dev/mmcblk0p22 became /dev/mmcblkp and
    # /dev/nvme0n1p2 became /dev/nvmenp.  Preserve the disk number and strip
    # only a trailing partition suffix on those naming schemes.
    old_device = """                            name = block.get_property('device')
                            name = ''.join([i for i in name if not i.isdigit()])
"""
    new_device = """                            name = block.get_property('device')
                            if name.startswith(('/dev/mmcblk', '/dev/nvme')):
                                base, separator, suffix = name.rpartition('p')
                                if separator and suffix.isdigit():
                                    name = base
                            else:
                                name = name.rstrip('0123456789')
"""
    if old_device in text:
        text = replace_once(text, old_device, new_device, "device-normalisation")
    elif "name = name.rstrip('0123456789')" not in text:
        raise SystemExit("mintstick integration: device-normalisation anchor missing")

    old_cli = """            usb_path = ''.join([i for i in a if not i.isdigit()])
"""
    new_cli = """            if a.startswith(('/dev/mmcblk', '/dev/nvme')):
                base, separator, suffix = a.rpartition('p')
                usb_path = base if separator and suffix.isdigit() else a
            else:
                usb_path = a.rstrip('0123456789')
"""
    if old_cli in text:
        text = replace_once(text, old_cli, new_cli, "cli-device-normalisation")
    elif "usb_path = a.rstrip('0123456789')" not in text:
        raise SystemExit("mintstick integration: cli-device-normalisation anchor missing")

    return text


def patch_formatter(text: str) -> str:
    # Stock Mintstick deliberately ignores subprocess return codes.  That can
    # produce its "formatted successfully" page even when dd, parted, wipefs or
    # mkfs all failed.  Make every destructive step fail closed.
    old_execute = """def execute(command):
    syslog.syslog(str(command))
    call(command)
    call(["sync"])
"""
    new_execute = """def execute(command):
    syslog.syslog(str(command))
    return_code = call(command)
    if return_code != 0:
        syslog.syslog("Command failed with exit status %d: %s" % (return_code, command))
        sys.exit(1)
    return_code = call(["sync"])
    if return_code != 0:
        syslog.syslog("sync failed with exit status %d" % return_code)
        sys.exit(1)
"""
    if old_execute in text:
        text = replace_once(text, old_execute, new_execute, "command-status")
    elif "Command failed with exit status" not in text:
        raise SystemExit("mintstick integration: command-status anchor missing")

    # Partition 1 is /dev/sdb1 on traditional SCSI-style names, but devices
    # whose whole-disk name ends in a digit use /dev/mmcblk0p1,
    # /dev/nvme0n1p1, and similar.  Use the kernel naming rule rather than the
    # stock hard-coded "<device>1" path.
    old_partition_path = '    partition_path = "%s1" % device_path\n'
    new_partition_path = (
        '    partition_suffix = "p1" if device_path[-1].isdigit() else "1"\n'
        '    partition_path = "%s%s" % (device_path, partition_suffix)\n'
    )
    if old_partition_path in text:
        text = replace_once(
            text, old_partition_path, new_partition_path, "partition-path"
        )
    elif 'partition_suffix = "p1" if device_path[-1].isdigit() else "1"' not in text:
        raise SystemExit("mintstick integration: partition-path anchor missing")

    # Force the kernel to adopt the new partition table and wait for the
    # resulting node before wipefs/mkfs.  A stale table must be reported as an
    # error rather than returning a false success.
    old_mkpart = '''    execute(["parted", device_path, "mkpart", "primary", partition_type, "1MiB", "100%"])

    # Call wipefs on the new partitions to avoid problems with old filesystem signatures
'''
    new_mkpart = '''    execute(["parted", device_path, "mkpart", "primary", partition_type, "1MiB", "100%"])
    execute(["partprobe", device_path])
    execute(["udevadm", "settle", "--timeout=10"])

    # Call wipefs on the new partitions to avoid problems with old filesystem signatures
'''
    if old_mkpart in text:
        text = replace_once(text, old_mkpart, new_mkpart, "partition-refresh")
    elif 'execute(["partprobe", device_path])' not in text:
        raise SystemExit("mintstick integration: partition-refresh anchor missing")

    if '"infiltratorfs"' not in text or "mkfs.infiltratorfs" not in text:
        old_partition = '''    elif fstype == "ext4":
        partition_type = "ext4"
'''
        new_partition = old_partition + '''    elif fstype == "infiltratorfs":
        partition_type = "ext4"
'''
        text = replace_once(text, old_partition, new_partition, "partition-type")

        old_mkfs = '''    elif fstype == "ext4":
        execute(["mkfs.ext4", "-E", "root_owner=%s:%s" % (uid, gid), "-L", volume_label, partition_path])
'''
        new_mkfs = old_mkfs + '''    elif fstype == "infiltratorfs":
        execute(["mkfs.infiltratorfs", "--force", "-L", volume_label, partition_path])
'''
        text = replace_once(text, old_mkfs, new_mkfs, "mkfs-command")

        old_choices = 'choices=("fat32", "exfat", "ntfs", "ext4")'
        new_choices = 'choices=("fat32", "exfat", "ntfs", "ext4", "infiltratorfs")'
        text = replace_once(text, old_choices, new_choices, "argparse-choices")

    return text


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("stock_ui")
    parser.add_argument("stock_formatter")
    parser.add_argument("output_ui")
    parser.add_argument("output_formatter")
    args = parser.parse_args()

    ui_src = Path(args.stock_ui)
    formatter_src = Path(args.stock_formatter)
    ui_out = Path(args.output_ui)
    formatter_out = Path(args.output_formatter)

    ui_out.write_text(patch_ui(ui_src.read_text(encoding="utf-8")), encoding="utf-8")
    formatter_out.write_text(
        patch_formatter(formatter_src.read_text(encoding="utf-8")), encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
