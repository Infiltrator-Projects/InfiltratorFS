#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path

root = Path('.')
kernel = root / 'kernel'
codec_path = kernel / 'infiltratorfs_linux_meta_codec.c'
meta_path = kernel / 'infiltratorfs_linux_meta.inc'
tool_path = root / 'tools/kernel-linux-meta-codec-object-refactor.py'

codec = codec_path.read_text()
meta = meta_path.read_text()

start_marker = '/* O(1) UUID-sidecar lookup after one bounded per-mount cache build. */\n'
end_marker = 'int infilfs_linux_meta_validate_blob('
if codec.count(start_marker) != 1 or codec.count(end_marker) != 1:
    raise SystemExit('unexpected current Linux metadata codec layout')
start = codec.index(start_marker)
end = codec.index(end_marker, start)
cache_block = codec[start:end]
codec = codec[:start] + codec[end:]

insert_marker = 'static int infilfs_linux_meta_find_child('
if meta.count(insert_marker) != 1:
    raise SystemExit('Linux metadata cache reinsertion anchor not found exactly once')
if start_marker in meta:
    raise SystemExit('Linux metadata cache already restored')
insert = meta.index(insert_marker)
meta = meta[:insert] + cache_block + meta[insert:]

codec_path.write_text(codec)
meta_path.write_text(meta)

# Fix the reusable extractor so future runs move only UUID/init plus the
# genuinely pure blob/xattr codec helpers.  The per-mount lookup cache remains
# with the VFS/namespace adapter until that adapter receives its own explicit
# compiled-object boundary.
tool = tool_path.read_text()
old = "    'static int infilfs_linux_meta_find_child(')"
new = "    '/* O(1) UUID-sidecar lookup after one bounded per-mount cache build. */')"
if tool.count(old) != 1:
    raise SystemExit('refactor tool UUID extraction boundary not found')
tool_path.write_text(tool.replace(old, new, 1))

# Boundary assertions: cache stays composite/private; codec object contains
# only helpers that do not own namespace mutation, transaction locking or the
# per-mount metadata directory cache.
fixed_codec = codec_path.read_text()
fixed_meta = meta_path.read_text()
for symbol in (
    'infilfs_linux_meta_cache_lookup',
    'infilfs_linux_meta_cache_prepare',
    'infilfs_linux_meta_cache_reset',
    'infilfs_linux_meta_cache_destroy',
    'infilfs_linux_meta_cache_insert',
    'infilfs_linux_meta_cache_remove',
    'infilfs_linux_meta_cache_build',
):
    if symbol in fixed_codec:
        raise SystemExit(f'cache symbol leaked into codec object: {symbol}')
    if symbol not in fixed_meta:
        raise SystemExit(f'cache symbol missing from VFS adapter: {symbol}')

for symbol in (
    'void infilfs_linux_meta_uuid(',
    'void infilfs_linux_meta_init(',
    'int infilfs_linux_meta_validate_blob(',
    'int infilfs_linux_meta_find_xattr(',
    'int infilfs_linux_xattr_name(',
):
    if symbol not in fixed_codec:
        raise SystemExit(f'pure codec symbol missing: {symbol}')

print('Linux metadata codec/cache boundary repaired.')
