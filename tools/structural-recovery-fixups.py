#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
mode = sys.argv[1] if len(sys.argv) > 1 else ""


def replace_once(path, old, new, label):
    p = ROOT / path
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one occurrence, found {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


if mode == "pre":
    replace_once(
        "tools/structural-recovery-phase1.py",
        '"static void mutation_scope_fail(struct infs_volume *vol, int started_transaction)",',
        '"static void mutation_scope_fail(struct infs_volume *vol,",',
        "line-wrapped mutation_scope_fail")
elif mode == "post":
    volume = ROOT / "src/volume.c"
    text = volume.read_text(encoding="utf-8")
    marker = "static infs_status file_free_unshared_run(\n    struct infs_volume *vol, const uint8_t owner_id[16],\n    uint64_t start, uint64_t count);\n"
    declarations = marker + "static void shared_ref_index_invalidate(struct infs_volume *vol);\nstatic void shared_ref_index_destroy(struct infs_volume *vol);\n"
    if "static void shared_ref_index_invalidate(struct infs_volume *vol);" not in text:
        if text.count(marker) != 1:
            raise SystemExit("shared-reference forward-declaration anchor mismatch")
        text = text.replace(marker, declarations, 1)
        volume.write_text(text, encoding="utf-8")

    recovery = ROOT / "src/volume/checkpoint-recovery.inc"
    text = recovery.read_text(encoding="utf-8")
    anchor = "static void discard_candidate_state(struct infs_volume *vol)\n{\n    free_extent_index_destroy(vol);\n"
    replacement = "static void discard_candidate_state(struct infs_volume *vol)\n{\n    shared_ref_index_destroy(vol);\n    free_extent_index_destroy(vol);\n"
    if "shared_ref_index_destroy(vol);" not in text:
        if text.count(anchor) != 1:
            raise SystemExit("shared-reference lifecycle anchor mismatch")
        text = text.replace(anchor, replacement, 1)
        recovery.write_text(text, encoding="utf-8")
else:
    raise SystemExit("usage: structural-recovery-fixups.py pre|post")
