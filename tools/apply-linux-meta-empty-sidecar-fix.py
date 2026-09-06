#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Apply the bounded deferred Linux metadata lifecycle repair.

This is a temporary release-engineering helper.  It deliberately fails closed
unless the exact audited 0.18.45-dev metadata store is present.
"""
from pathlib import Path

path = Path("kernel/infiltratorfs_linux_meta.inc")
text = path.read_text()
old = '''    if (empty)\n        return infilfs_linux_meta_remove_object_locked(sb, object_id);\n\n    ret = infilfs_linux_meta_get_file(sb, object_id, true,\n                                      &dir, &file, name);\n    if (ret)\n        return ret;\n'''
new = '''    /*\n     * Do not delete and recreate a sidecar merely because its last Linux\n     * metadata record was removed.  Deferred native transactions may update\n     * the same target repeatedly (quota policy and ACL churn are common root\n     * filesystem examples).  Deleting and recreating the UUID sidecar inside\n     * one still-private transaction needlessly exercises remove/add index\n     * topology and previously left a corrupt published image.\n     *\n     * Keep an existing empty sidecar as the canonical 32-byte header instead.\n     * A target that never had a sidecar still stays allocation-free.  Actual\n     * target deletion continues to remove its sidecar through\n     * infilfs_linux_meta_remove_object(), so this does not orphan metadata.\n     */\n    if (empty) {\n        ret = infilfs_linux_meta_get_file(sb, object_id, false,\n                                          &dir, &file, name);\n        if (ret == -ENOENT)\n            return 0;\n    } else {\n        ret = infilfs_linux_meta_get_file(sb, object_id, true,\n                                          &dir, &file, name);\n    }\n    if (ret)\n        return ret;\n'''
count = text.count(old)
if count != 1:
    raise SystemExit(f"expected exactly one audited metadata-store block, found {count}")
text = text.replace(old, new, 1)
path.write_text(text)

# Strengthen the static scaling policy so a future cleanup cannot silently
# reintroduce delete/recreate churn into the hot metadata path.
test = Path("tests/native-small-file-scaling-policy.sh")
t = test.read_text()
needle = "! grep -Fq 'infilfs_native_pending_flush_sb' \"$meta\"\n"
addition = "grep -Fq 'if (empty) {' \"$meta\"\ngrep -Fq 'infilfs_linux_meta_get_file(sb, object_id, false' \"$meta\"\n"
if addition not in t:
    if t.count(needle) != 1:
        raise SystemExit("metadata scaling policy anchor changed")
    t = t.replace(needle, needle + addition, 1)
    test.write_text(t)

print("Linux metadata empty-sidecar lifecycle repair applied.")
