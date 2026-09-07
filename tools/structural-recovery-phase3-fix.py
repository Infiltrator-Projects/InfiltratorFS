#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Add the forward declaration required by the native chain builder."""
from pathlib import Path

path = Path(__file__).resolve().parents[1] / "kernel/infiltratorfs_rw_data.inc"
text = path.read_text(encoding="utf-8")
anchor = "static int infilfs_native_build_file_object(\n"
prototype = '''static int infilfs_native_build_extent_page(\n    struct infilfs_native_pending *pending, const u8 owner_id[16],\n    const struct infilfs_extent_disk *extents, u32 count, u64 next_block,\n    u8 page_block[INFILFS_DISK_BLOCK_SIZE]);\n\n'''
if prototype not in text:
    if anchor not in text:
        raise SystemExit("native file-object builder anchor not found")
    text = text.replace(anchor, prototype + anchor, 1)
path.write_text(text, encoding="utf-8")
print("Native extent-page builder forward declaration applied.")
