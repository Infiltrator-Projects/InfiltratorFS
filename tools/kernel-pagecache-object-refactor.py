#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path

root = Path('.')
kernel = root / 'kernel'
core = kernel / 'infiltratorfs_core.c'
internal = kernel / 'infiltratorfs_internal.h'
rw = kernel / 'infiltratorfs_rw.inc'
data_path = kernel / 'infiltratorfs_rw_data.inc'
old_pagecache = kernel / 'infiltratorfs_pagecache.inc'
new_pagecache = kernel / 'infiltratorfs_pagecache.c'
makefile = kernel / 'Makefile'
maint = root / 'tests/native-kernel-maintainability-policy.sh'
readme = kernel / 'README.md'

assert old_pagecache.exists() and not new_pagecache.exists()

# Page-cache writeback calls the native extent writer directly. Give that
# already-shared helper ordinary module-private linkage rather than relying on
# textual composition.
dt = data_path.read_text()
old = 'static ssize_t infilfs_native_extent_write_iter(struct inode *inode,'
assert dt.count(old) == 1
dt = dt.replace(old, 'ssize_t infilfs_native_extent_write_iter(struct inode *inode,', 1)
data_path.write_text(dt)

# Core consumes the address-space operations table through the private header.
ct = core.read_text()
old = 'static const struct address_space_operations infilfs_aops;\n'
assert ct.count(old) == 1
core.write_text(ct.replace(old, '', 1))

# Build the independently compiled page-cache component.
pc = old_pagecache.read_text()
pc = pc.replace('// SPDX-License-Identifier: GPL-3.0-or-later\n',
                '// SPDX-License-Identifier: GPL-3.0-or-later\n#include "infiltratorfs_internal.h"\n', 1)
old = 'static const struct address_space_operations infilfs_aops = {'
assert pc.count(old) == 1
pc = pc.replace(old, 'const struct address_space_operations infilfs_aops = {', 1)
new_pagecache.write_text(pc)
old_pagecache.unlink()

# Private module API: one write iterator plus the VFS a_ops table.
h = internal.read_text()
anchor = 'ssize_t infilfs_file_read_iter_cached(struct kiocb *iocb, struct iov_iter *to);\n'
assert anchor in h
api = '''ssize_t infilfs_native_extent_write_iter(
    struct inode *inode, loff_t *position, struct iov_iter *from,
    size_t requested);
extern const struct address_space_operations infilfs_aops;
'''
h = h.replace(anchor, anchor + api, 1)
internal.write_text(h)

# Remove the old textual composition from rw.inc.
rwt = rw.read_text()
inc = '#include "infiltratorfs_pagecache.inc"\n'
assert inc in rwt
rw.write_text(rwt.replace(inc, '', 1))

# Kbuild object eight and component contract.
mk = makefile.read_text()
old_line = ('infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o '
            'infiltratorfs_resize.o infiltratorfs_index_tree.o '
            'infiltratorfs_parallel_alloc.o infiltratorfs_allocation_publish.o '
            'infiltratorfs_read_cache.o')
new_line = old_line + ' infiltratorfs_pagecache.o'
assert old_line in mk
mk = mk.replace(old_line, new_line, 1)
marker = '#   infiltratorfs_read_cache.c owns verified read cursor/page caching.\n'
assert marker in mk
mk = mk.replace(marker, marker +
    '#   infiltratorfs_pagecache.c owns Linux folio/page-cache integration.\n', 1)
makefile.write_text(mk)

# Package/DKMS/source manifests now ship pagecache.c instead of the include.
for path in root.rglob('*'):
    if not path.is_file() or '.git' in path.parts or path == Path('tools/kernel-pagecache-object-refactor.py'):
        continue
    if path.suffix not in {'.sh', '.yml', '.yaml', '.md', '.txt', '.in', '.c', '.h'}:
        continue
    try:
        text = path.read_text()
    except UnicodeDecodeError:
        continue
    changed = text.replace('infiltratorfs_pagecache.inc', 'infiltratorfs_pagecache.c')
    if changed != text:
        path.write_text(changed)

# Maintainability policy: exact eight-object layout and no textual page-cache
# regression. The global filename migration updates the old ordered entry to
# .c; remove it because compiled objects are not part of the include-order list.
mp = maint.read_text()
old_guard = "grep -Fqx 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o infiltratorfs_resize.o infiltratorfs_index_tree.o infiltratorfs_parallel_alloc.o infiltratorfs_allocation_publish.o infiltratorfs_read_cache.o' \"$makefile\" || \\\n"
new_guard = "grep -Fqx 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o infiltratorfs_resize.o infiltratorfs_index_tree.o infiltratorfs_parallel_alloc.o infiltratorfs_allocation_publish.o infiltratorfs_read_cache.o infiltratorfs_pagecache.o' \"$makefile\" || \\\n"
assert old_guard in mp
mp = mp.replace(old_guard, new_guard, 1)
anchor = "test -f \"$kernel/infiltratorfs_read_cache.c\" || fail 'verified-read cache object missing'\n"
assert anchor in mp
mp = mp.replace(anchor, anchor +
    "test -f \"$kernel/infiltratorfs_pagecache.c\" || fail 'page-cache object missing'\n"
    "test ! -e \"$kernel/infiltratorfs_pagecache.inc\" || fail 'page-cache regressed to textual include'\n"
    "! grep -Fq 'infiltratorfs_pagecache.inc' \"$rw\" || fail 'RW compositor textually includes page cache'\n", 1)
mp = mp.replace('    infiltratorfs_pagecache.c\n', '', 1)
maint.write_text(mp)

# Local architecture prose follows the compiled boundary.
doc = readme.read_text()
old = ('`infiltratorfs_allocation_publish.c` owns CoW allocation-tree publication, and '
       '`infiltratorfs_read_cache.c` owns verified read cursor/page caching.')
new = ('`infiltratorfs_allocation_publish.c` owns CoW allocation-tree publication, '
       '`infiltratorfs_read_cache.c` owns verified read cursor/page caching, and '
       '`infiltratorfs_pagecache.c` owns Linux folio/page-cache integration.')
assert old in doc
doc = doc.replace(old, new, 1)
doc = doc.replace(
    'including write-data paths, namespace mutation, directory trees, page cache, Linux metadata, quotas and defragmentation.',
    'including write-data paths, namespace mutation, directory trees, Linux metadata, quotas and defragmentation.',
    1)
doc = doc.replace(
    'prevents extracted allocation/index/resize/reservation/publication/read-cache components from regressing into textual inclusion,',
    'prevents extracted allocation/index/resize/reservation/publication/read-cache/page-cache components from regressing into textual inclusion,',
    1)
readme.write_text(doc)

# Old filename may survive only in this one-shot helper and negative guards.
stale = []
for path in root.rglob('*'):
    if not path.is_file() or '.git' in path.parts or path in {
        Path('tools/kernel-pagecache-object-refactor.py'),
        Path('tests/native-kernel-maintainability-policy.sh'),
    }:
        continue
    try:
        text = path.read_text()
    except UnicodeDecodeError:
        continue
    if 'infiltratorfs_pagecache.inc' in text:
        stale.append(str(path))
assert not stale, stale
