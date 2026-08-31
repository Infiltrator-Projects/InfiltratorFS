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
    if '"infiltratorfs"' in text:
        return text
    old = '            self.fsmodel.append(["ext4", "EXT4", 16, False, False])'
    new = (
        old
        + '\n            self.fsmodel.append('
        + '["infiltratorfs", "InfiltratorFS", 63, False, False])'
    )
    return replace_once(text, old, new, "filesystem-list")


def patch_formatter(text: str) -> str:
    if '"infiltratorfs"' in text and "mkfs.infiltratorfs" in text:
        return text

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
    return replace_once(text, old_choices, new_choices, "argparse-choices")


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
