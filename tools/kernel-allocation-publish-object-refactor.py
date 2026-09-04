#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path

root = Path('.')
kernel = root / 'kernel'
core = kernel / 'infiltratorfs_core.c'
internal = kernel / 'infiltratorfs_internal.h'
rw = kernel / 'infiltratorfs_rw.inc'
legacy = kernel / 'infiltratorfs_rw_legacy.inc'
old_publish = kernel / 'infiltratorfs_allocation_publish.inc'
new_publish = kernel / 'infiltratorfs_allocation_publish.c'
makefile = kernel / 'Makefile'
maint = Path('tests/native-kernel-maintainability-policy.sh')
readme = kernel / 'README.md'

assert old_publish.exists() and not new_publish.exists()

# The publication engine becomes a normal translation unit. Its single public
# transaction entry point and already-shared page writer stay module-private.
p = old_publish.read_text()
p = p.replace('// SPDX-License-Identifier: GPL-3.0-or-later\n',
              '// SPDX-License-Identifier: GPL-3.0-or-later\n#include "infiltratorfs_internal.h"\n', 1)
old = 'static int infilfs_rw_allocation_map_publish('
assert p.count(old) == 1
p = p.replace(old, 'int infilfs_rw_allocation_map_publish(', 1)
new_publish.write_text(p)
old_publish.unlink()

# Transaction primitives used by the publication object already serve many RW
# layers. Give them explicit module-private linkage instead of relying on one TU.
lt = legacy.read_text()
forward = '''static int infilfs_rw_allocation_map_publish(
    struct infilfs_rw_tx *tx,
    struct infilfs_allocation_layout *next_layout);
'''
assert forward in lt
lt = lt.replace(forward, '', 1)
for old, new in [
    ('static u64 infilfs_rw_crc64_zeroed(', 'u64 infilfs_rw_crc64_zeroed('),
    ('static int infilfs_rw_tx_alloc(', 'int infilfs_rw_tx_alloc('),
    ('static int infilfs_rw_tx_defer_free(', 'int infilfs_rw_tx_defer_free('),
    ('static int infilfs_rw_tx_apply_deferred(', 'int infilfs_rw_tx_apply_deferred('),
]:
    assert lt.count(old) == 1, (old, lt.count(old))
    lt = lt.replace(old, new, 1)
legacy.write_text(lt)

# Remove the now-redundant static CRC prototype in the core; the private header
# becomes the authoritative cross-object declaration.
ct = core.read_text()
proto = '''static u64 infilfs_rw_crc64_zeroed(const u8 *data, size_t length,
                                    size_t zero_offset, size_t zero_length);
'''
assert proto in ct
core.write_text(ct.replace(proto, '', 1))

header = internal.read_text()
anchor = '''int infilfs_parallel_consume_reservation(
    struct infilfs_rw_tx *tx, struct infilfs_parallel_reservation *reservation,
    u64 count, u64 *start_out);
'''
assert anchor in header
api = '''
/* Transaction services shared with the compiled allocation publisher. */
u64 infilfs_rw_crc64_zeroed(
    const u8 *data, size_t length, size_t zero_offset, size_t zero_length);
int infilfs_rw_tx_alloc(struct infilfs_rw_tx *tx, u64 count, u64 *start_out);
int infilfs_rw_tx_defer_free(struct infilfs_rw_tx *tx, u64 start, u64 count);
int infilfs_rw_tx_apply_deferred(struct infilfs_rw_tx *tx);
int infilfs_rw_allocation_map_publish(
    struct infilfs_rw_tx *tx, struct infilfs_allocation_layout *next_layout);
'''
header = header.replace(anchor, anchor + api, 1)
internal.write_text(header)

# Remove textual publication composition.
rwt = rw.read_text()
inc = '#include "infiltratorfs_allocation_publish.inc"\n'
assert inc in rwt
rw.write_text(rwt.replace(inc, '', 1))

# Kbuild object six.
mk = makefile.read_text()
old_line = ('infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o '
            'infiltratorfs_resize.o infiltratorfs_index_tree.o '
            'infiltratorfs_parallel_alloc.o')
new_line = old_line + ' infiltratorfs_allocation_publish.o'
assert old_line in mk
mk = mk.replace(old_line, new_line, 1)
marker = '#   infiltratorfs_parallel_alloc.c owns volatile sharded data reservations.\n'
assert marker in mk
mk = mk.replace(marker, marker +
    '#   infiltratorfs_allocation_publish.c owns CoW allocation-tree publication.\n', 1)
makefile.write_text(mk)

# Package, DKMS and policy manifests follow the renamed source unit.
for path in root.rglob('*'):
    if not path.is_file() or '.git' in path.parts or path == Path('tools/kernel-allocation-publish-object-refactor.py'):
        continue
    if path.suffix not in {'.sh', '.yml', '.yaml', '.md', '.txt', '.in', '.c', '.h'}:
        continue
    try:
        text = path.read_text()
    except UnicodeDecodeError:
        continue
    changed = text.replace('infiltratorfs_allocation_publish.inc',
                           'infiltratorfs_allocation_publish.c')
    if changed != text:
        path.write_text(changed)

# Maintainability: exact six-object layout and remove publication from the RW
# textual-order contract after the global filename migration.
mp = maint.read_text()
old_guard = "grep -Fqx 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o infiltratorfs_resize.o infiltratorfs_index_tree.o infiltratorfs_parallel_alloc.o' \"$makefile\" || \\\n"
new_guard = "grep -Fqx 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o infiltratorfs_resize.o infiltratorfs_index_tree.o infiltratorfs_parallel_alloc.o infiltratorfs_allocation_publish.o' \"$makefile\" || \\\n"
assert old_guard in mp
mp = mp.replace(old_guard, new_guard, 1)
anchor = "test -f \"$kernel/infiltratorfs_parallel_alloc.c\" || fail 'parallel allocator object missing'\n"
assert anchor in mp
mp = mp.replace(anchor, anchor +
    "test -f \"$kernel/infiltratorfs_allocation_publish.c\" || fail 'allocation publisher object missing'\n"
    "test ! -e \"$kernel/infiltratorfs_allocation_publish.inc\" || fail 'allocation publisher regressed to textual include'\n"
    "! grep -Fq 'infiltratorfs_allocation_publish.inc' \"$rw\" || fail 'RW compositor textually includes allocation publisher'\n", 1)
mp = mp.replace('    infiltratorfs_allocation_publish.c\n', '', 1)
maint.write_text(mp)

# Update local architecture prose.
doc = readme.read_text()
old = ('`infiltratorfs_index_tree.c` owns Format 0.17 object-index reads and snapshots, and '
       '`infiltratorfs_parallel_alloc.c` owns volatile sharded data reservations.')
new = ('`infiltratorfs_index_tree.c` owns Format 0.17 object-index reads and snapshots, '
       '`infiltratorfs_parallel_alloc.c` owns volatile sharded data reservations, and '
       '`infiltratorfs_allocation_publish.c` owns CoW allocation-tree publication.')
assert old in doc
doc = doc.replace(old, new, 1)
doc = doc.replace(
    'including allocation publication, read/write data paths, namespace mutation, directory trees, page cache, Linux metadata, quotas and defragmentation.',
    'including read/write data paths, namespace mutation, directory trees, page cache, Linux metadata, quotas and defragmentation.',
    1)
doc = doc.replace(
    'prevents extracted allocation/index/resize/reservation components from regressing into textual inclusion,',
    'prevents extracted allocation/index/resize/reservation/publication components from regressing into textual inclusion,',
    1)
readme.write_text(doc)

# Old filename is allowed only in this helper and intentional negative guards.
stale = []
for path in root.rglob('*'):
    if not path.is_file() or '.git' in path.parts or path in {
        Path('tools/kernel-allocation-publish-object-refactor.py'),
        Path('tests/native-kernel-maintainability-policy.sh'),
    }:
        continue
    try:
        text = path.read_text()
    except UnicodeDecodeError:
        continue
    if 'infiltratorfs_allocation_publish.inc' in text:
        stale.append(str(path))
assert not stale, stale
