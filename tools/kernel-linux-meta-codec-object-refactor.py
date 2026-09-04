#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import re

root = Path('.')
kernel = root / 'kernel'
meta = kernel / 'infiltratorfs_linux_meta.inc'
codec = kernel / 'infiltratorfs_linux_meta_codec.c'
internal = kernel / 'infiltratorfs_internal.h'
makefile = kernel / 'Makefile'
maint = root / 'tests/native-kernel-maintainability-policy.sh'
readme = kernel / 'README.md'

assert meta.exists() and not codec.exists()

text = meta.read_text()

def take_before(source, start_marker, next_marker):
    start = source.index(start_marker)
    end = source.index(next_marker, start)
    return source[start:end], source[:start] + source[end:]

# Move the stable Linux-sidecar record layout into the module-private header.
# It is already consumed by quota code later in the same core translation unit;
# making it explicit removes that include-order dependency without changing the
# portable on-disk filesystem format.
layout_block, text = take_before(
    text,
    '#define INFILFS_LINUX_META_MAGIC "INPSXM01"',
    'static void infilfs_linux_meta_uuid(')

# Pure codec/naming helpers become independently compiled services.  They do
# not perform namespace mutation, take filesystem locks or publish transactions.
uuid_block, text = take_before(
    text,
    'static void infilfs_linux_meta_uuid(',
    'static int infilfs_linux_meta_find_child(')
validate_block, text = take_before(
    text,
    'static int infilfs_linux_meta_validate_blob(',
    'static int infilfs_linux_meta_load(')
find_block, text = take_before(
    text,
    'static int infilfs_linux_meta_find_xattr(',
    'static int infilfs_linux_meta_get_special(')
name_block, text = take_before(
    text,
    'static int infilfs_linux_xattr_name(',
    'static int infilfs_linux_xattr_get(')

for old, new in [
    ('static void infilfs_linux_meta_uuid(', 'void infilfs_linux_meta_uuid('),
    ('static void infilfs_linux_meta_init(', 'void infilfs_linux_meta_init('),
    ('static int infilfs_linux_meta_validate_blob(', 'int infilfs_linux_meta_validate_blob('),
    ('static int infilfs_linux_meta_find_xattr(', 'int infilfs_linux_meta_find_xattr('),
    ('static int infilfs_linux_xattr_name(', 'int infilfs_linux_xattr_name('),
]:
    for block_name in ('uuid_block', 'validate_block', 'find_block', 'name_block'):
        value = locals()[block_name]
        if old in value:
            locals()[block_name] = value.replace(old, new, 1)
            break
    else:
        raise AssertionError(old)

# Python's locals() assignment is not guaranteed to update local variables;
# perform the linkage replacements explicitly on the composed source as well.
codec_body = uuid_block + validate_block + find_block + name_block
for old, new in [
    ('static void infilfs_linux_meta_uuid(', 'void infilfs_linux_meta_uuid('),
    ('static void infilfs_linux_meta_init(', 'void infilfs_linux_meta_init('),
    ('static int infilfs_linux_meta_validate_blob(', 'int infilfs_linux_meta_validate_blob('),
    ('static int infilfs_linux_meta_find_xattr(', 'int infilfs_linux_meta_find_xattr('),
    ('static int infilfs_linux_xattr_name(', 'int infilfs_linux_xattr_name('),
]:
    assert codec_body.count(old) == 1, (old, codec_body.count(old))
    codec_body = codec_body.replace(old, new, 1)

meta.write_text(text)
codec.write_text(
    '// SPDX-License-Identifier: GPL-3.0-or-later\n'
    '#include "infiltratorfs_internal.h"\n\n'
    + codec_body)

# Private header owns the sidecar layout and codec API.
h = internal.read_text()
anchor = '#define INFILFS_LINUX_META_DIRECTORY ".infilfs-posix-meta"\n'
assert anchor in h
h = h.replace(anchor, anchor + '\n' + layout_block, 1)
api_anchor = 'int infilfs_native_tree_directory_update(\n'
api_pos = h.index(api_anchor)
# Put codec declarations immediately before the directory-tree API tail so the
# private interface remains grouped by compiled component.
api = '''/* Pure Linux sidecar metadata codec; no namespace or locking ownership. */
void infilfs_linux_meta_uuid(const u8 id[16], char out[37]);
void infilfs_linux_meta_init(struct infilfs_linux_meta_header *header);
int infilfs_linux_meta_validate_blob(const u8 *blob, size_t size);
int infilfs_linux_meta_find_xattr(
    const u8 *blob, size_t size, const char *name, size_t *offset_out,
    size_t *record_size_out, size_t *value_offset_out,
    size_t *value_length_out);
int infilfs_linux_xattr_name(
    const struct xattr_handler *handler, const char *name, char **full_out);

'''
h = h[:api_pos] + api + h[api_pos:]
internal.write_text(h)

# Kbuild object ten.
mk = makefile.read_text()
old_line = ('infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o '
            'infiltratorfs_resize.o infiltratorfs_index_tree.o '
            'infiltratorfs_parallel_alloc.o infiltratorfs_allocation_publish.o '
            'infiltratorfs_read_cache.o infiltratorfs_pagecache.o '
            'infiltratorfs_directory_tree.o')
new_line = old_line + ' infiltratorfs_linux_meta_codec.o'
assert old_line in mk
mk = mk.replace(old_line, new_line, 1)
marker = '#   infiltratorfs_directory_tree.c owns scalable Format 0.17 directory trees.\n'
assert marker in mk
mk = mk.replace(marker, marker +
    '#   infiltratorfs_linux_meta_codec.c owns Linux sidecar blob/naming codec rules.\n', 1)
makefile.write_text(mk)

# Update DKMS/package source manifests.  Keep linux_meta.inc itself because the
# VFS/namespace adapter remains textually composed for now; add the codec source
# beside every shipped occurrence without disturbing quoted one-path checks.
manifest_paths = [
    root / '.github/workflows/kernel-module.yml',
    root / 'packaging/build-linux-packages.sh',
    root / 'tests/native-complete-qualification.sh',
]
filename = 'infiltratorfs_linux_meta.inc'
codec_name = 'infiltratorfs_linux_meta_codec.c'
path_pattern = re.compile(r'(?P<prefix>(?:[A-Za-z0-9_.$+{}-]+/)*)' + re.escape(filename))
for path in manifest_paths:
    data = path.read_text()
    out = []
    changed = False
    for line in data.splitlines(keepends=True):
        if filename not in line:
            out.append(line)
            continue
        idx = line.index(filename)
        left_quote = line.rfind('"', 0, idx)
        right_quote = line.find('"', idx + len(filename))
        # Exact quoted path/pattern lines are safest duplicated, preserving all
        # shell quoting and regex suffixes.
        if left_quote >= 0 and right_quote >= 0:
            out.append(line.replace(filename, codec_name, 1))
            out.append(line)
            changed = True
            continue
        def pair(match):
            prefix = match.group('prefix') or ''
            return (prefix + codec_name + ' ' + prefix + filename)
        replaced, count = path_pattern.subn(pair, line)
        assert count >= 1, (path, line)
        out.append(replaced)
        changed = True
    assert changed, f'no Linux metadata manifest entry found in {path}'
    path.write_text(''.join(out))

# Maintainability guard knows object ten exists while the Linux metadata adapter
# intentionally remains in the compositor.
mp = maint.read_text()
old_guard = "grep -Fqx 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o infiltratorfs_resize.o infiltratorfs_index_tree.o infiltratorfs_parallel_alloc.o infiltratorfs_allocation_publish.o infiltratorfs_read_cache.o infiltratorfs_pagecache.o infiltratorfs_directory_tree.o' \"$makefile\" || \\\n"
new_guard = "grep -Fqx 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o infiltratorfs_resize.o infiltratorfs_index_tree.o infiltratorfs_parallel_alloc.o infiltratorfs_allocation_publish.o infiltratorfs_read_cache.o infiltratorfs_pagecache.o infiltratorfs_directory_tree.o infiltratorfs_linux_meta_codec.o' \"$makefile\" || \\\n"
assert old_guard in mp
mp = mp.replace(old_guard, new_guard, 1)
anchor = "test -f \"$kernel/infiltratorfs_directory_tree.c\" || fail 'directory-tree object missing'\n"
assert anchor in mp
mp = mp.replace(anchor, anchor +
    "test -f \"$kernel/infiltratorfs_linux_meta_codec.c\" || fail 'Linux metadata codec object missing'\n", 1)
maint.write_text(mp)

# Architecture prose distinguishes the newly compiled pure codec from the still
# composed Linux VFS/namespace adapter.
doc = readme.read_text()
old = ('`infiltratorfs_pagecache.c` owns Linux folio/page-cache integration, and '
       '`infiltratorfs_directory_tree.c` owns scalable Format 0.17 directory trees.')
new = ('`infiltratorfs_pagecache.c` owns Linux folio/page-cache integration, '
       '`infiltratorfs_directory_tree.c` owns scalable Format 0.17 directory trees, and '
       '`infiltratorfs_linux_meta_codec.c` owns Linux sidecar blob/naming codec rules.')
assert old in doc
doc = doc.replace(old, new, 1)
doc = doc.replace(
    'including write-data paths, namespace mutation, Linux metadata, quotas and defragmentation.',
    'including write-data paths, namespace mutation, the Linux metadata VFS/namespace adapter, quotas and defragmentation.',
    1)
readme.write_text(doc)

# The moved pure helpers must not remain duplicated in the adapter.
remaining = meta.read_text()
for forbidden in [
    'static void infilfs_linux_meta_uuid(',
    'static void infilfs_linux_meta_init(',
    'static int infilfs_linux_meta_validate_blob(',
    'static int infilfs_linux_meta_find_xattr(',
    'static int infilfs_linux_xattr_name(',
]:
    assert forbidden not in remaining, forbidden
