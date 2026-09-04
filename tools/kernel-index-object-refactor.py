#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path

root = Path('.')
kernel = root / 'kernel'
core = kernel / 'infiltratorfs_core.c'
internal = kernel / 'infiltratorfs_internal.h'
old_index = kernel / 'infiltratorfs_index_tree.inc'
new_index = kernel / 'infiltratorfs_index_tree.c'
makefile = kernel / 'Makefile'
policy = Path('tests/native-kernel-maintainability-policy.sh')
readme = kernel / 'README.md'

assert core.exists() and internal.exists() and old_index.exists()
assert not new_index.exists()

# Convert the already-qualified read-side object-index tree into a normal
# translation unit. Only its two entry points cross the object boundary.
index = old_index.read_text()
index = index.replace(
    '// SPDX-License-Identifier: GPL-3.0-or-later\n',
    '// SPDX-License-Identifier: GPL-3.0-or-later\n#include "infiltratorfs_internal.h"\n',
    1)
for old, new in [
    ('static int infilfs_index_tree_lookup_head(',
     'int infilfs_index_tree_lookup_head('),
    ('static int infilfs_index_tree_snapshot(',
     'int infilfs_index_tree_snapshot('),
]:
    assert old in index, old
    index = index.replace(old, new, 1)
new_index.write_text(index)
old_index.unlink()

# Remove textual composition from the core.
text = core.read_text()
assert '#include "infiltratorfs_index_tree.inc"\n' in text
text = text.replace('#include "infiltratorfs_index_tree.inc"\n', '', 1)

# The index object needs a small set of validation/read helpers and the bounded
# visit-set type. Make those private-module APIs explicit.
visit_struct = '''struct infilfs_visit_set {\n    u64 *slots;\n    size_t capacity;\n    size_t count;\n};\n\n'''
assert text.count(visit_struct) == 1
text = text.replace(visit_struct, '', 1)

for old, new, count in [
    ('static const u8 infilfs_index_page_magic[8]',
     'const u8 infilfs_index_page_magic[8]', 1),
    ('static const u8 infilfs_index_branch_page_magic[8]',
     'const u8 infilfs_index_branch_page_magic[8]', 1),
    ('static bool infilfs_block_allocated(',
     'bool infilfs_block_allocated(', 1),
    ('static int infilfs_visit_claim(',
     'int infilfs_visit_claim(', 1),
    ('static void infilfs_visit_destroy(',
     'void infilfs_visit_destroy(', 1),
    ('static bool infilfs_metadata_page_valid(',
     'bool infilfs_metadata_page_valid(', 1),
]:
    assert text.count(old) == count, (old, text.count(old))
    text = text.replace(old, new, count)

# read_allocated_block has both a forward declaration and a definition.
old = 'static int infilfs_read_allocated_block('
assert text.count(old) == 2, text.count(old)
text = text.replace(old, 'int infilfs_read_allocated_block(', 2)
core.write_text(text)

header = internal.read_text()
struct_anchor = '''struct infilfs_allocation_layout {\n    u64 *leaf_blocks;\n    u64 *branch_blocks;\n    size_t leaf_count;\n    size_t branch_count;\n    size_t level1_count;\n    size_t level2_count;\n};\n'''
assert struct_anchor in header
header = header.replace(
    struct_anchor,
    struct_anchor + '''\n/* Bounded cycle/alias detector shared by metadata-tree walkers. */\nstruct infilfs_visit_set {\n    u64 *slots;\n    size_t capacity;\n    size_t count;\n};\n''',
    1)

api_anchor = '''int infilfs_allocation_map_load(\n    struct super_block *sb, const struct infilfs_superblock_disk *disk,\n    u8 **bitmap_out, size_t *bitmap_bytes_out,\n    struct infilfs_allocation_layout *layout_out);\n'''
assert api_anchor in header
index_api = '''\n/* Services and entry points shared with the compiled object-index tree. */\nextern const u8 infilfs_index_page_magic[8];\nextern const u8 infilfs_index_branch_page_magic[8];\nbool infilfs_block_allocated(struct super_block *sb, u64 block);\nint infilfs_read_allocated_block(struct super_block *sb, u64 block, void *out);\nbool infilfs_metadata_page_valid(\n    struct super_block *sb, const u8 *block, const u8 magic[8],\n    const u8 owner_id[16]);\nint infilfs_visit_claim(struct infilfs_visit_set *set, u64 block);\nvoid infilfs_visit_destroy(struct infilfs_visit_set *set);\nint infilfs_index_tree_lookup_head(\n    struct super_block *sb, const u8 head[INFILFS_DISK_BLOCK_SIZE],\n    const u8 object_id[16], u64 *object_block_out, u16 *type_out);\nint infilfs_index_tree_snapshot(\n    struct super_block *sb, const u8 head[INFILFS_DISK_BLOCK_SIZE],\n    struct infilfs_index_entry_disk **entries_out, u32 *count_out);\n'''
header = header.replace(api_anchor, api_anchor + index_api, 1)
internal.write_text(header)

# Add the new object to Kbuild and document its ownership boundary.
mk = makefile.read_text()
old_kbuild = ('infiltratorfs-y := infiltratorfs_core.o '
              'infiltratorfs_allocation_map.o infiltratorfs_resize.o')
new_kbuild = (old_kbuild + ' infiltratorfs_index_tree.o')
assert old_kbuild in mk
mk = mk.replace(old_kbuild, new_kbuild, 1)
marker = '#   infiltratorfs_resize.c owns mounted geometry transitions.\n'
assert marker in mk
mk = mk.replace(
    marker,
    marker + '#   infiltratorfs_index_tree.c owns Format 0.17 object-index reads/snapshots.\n',
    1)
makefile.write_text(mk)

# All package/DKMS/source-inspection manifests now ship the compiled C unit.
for path in root.rglob('*'):
    if not path.is_file() or '.git' in path.parts or path == Path('tools/kernel-index-object-refactor.py'):
        continue
    if path.suffix not in {'.sh', '.yml', '.yaml', '.md', '.txt', '.in', '.c', '.h'}:
        continue
    try:
        data = path.read_text()
    except UnicodeDecodeError:
        continue
    changed = data.replace('infiltratorfs_index_tree.inc', 'infiltratorfs_index_tree.c')
    if changed != data:
        path.write_text(changed)

# Make the maintainability guard exact about all four compiled objects and stop
# the object-index tree from regressing to textual inclusion.
data = policy.read_text()
lines = data.splitlines()
for i, line in enumerate(lines):
    if line.startswith("grep -Fq 'infiltratorfs-y :="):
        lines[i] = ("grep -Fqx 'infiltratorfs-y := infiltratorfs_core.o "
                    "infiltratorfs_allocation_map.o infiltratorfs_resize.o "
                    "infiltratorfs_index_tree.o' \"$makefile\" || \\")
        if i + 1 < len(lines) and "fail 'kernel module is no longer built" in lines[i + 1]:
            pass
        else:
            lines.insert(i + 1, "    fail 'kernel module is no longer built from explicit component objects'")
        break
else:
    raise AssertionError('Kbuild maintainability check not found')
data = '\n'.join(lines) + '\n'
marker = "test -f \"$kernel/infiltratorfs_allocation_map.c\" || fail 'allocation map object missing'\n"
assert marker in data
data = data.replace(
    marker,
    marker +
    "test -f \"$kernel/infiltratorfs_index_tree.c\" || fail 'object-index tree object missing'\n"
    "test ! -e \"$kernel/infiltratorfs_index_tree.inc\" || fail 'object-index tree regressed to textual include'\n"
    "! grep -Fq 'infiltratorfs_index_tree.inc' \"$driver\" || fail 'core textually includes object-index tree'\n",
    1)
policy.write_text(data)

# Keep the kernel-local description aligned with the actual compiled boundary.
data = readme.read_text()
old = ('`infiltratorfs_core.c` owns VFS, mount and checkpoint orchestration, '
       '`infiltratorfs_allocation_map.c` owns Format 0.17 allocation-map loading, '
       'and `infiltratorfs_resize.c` owns mounted geometry transitions.')
new = ('`infiltratorfs_core.c` owns VFS, mount and checkpoint orchestration, '
       '`infiltratorfs_allocation_map.c` owns Format 0.17 allocation-map loading, '
       '`infiltratorfs_resize.c` owns mounted geometry transitions, and '
       '`infiltratorfs_index_tree.c` owns Format 0.17 object-index reads and snapshots.')
assert old in data
data = data.replace(old, new, 1)
data = data.replace(
    'including allocation publication, parallel reservation, read/write data paths, namespace mutation, indexes, directory trees, page cache, Linux metadata, resize, quotas and defragmentation.',
    'including allocation publication, parallel reservation, read/write data paths, namespace mutation, directory trees, page cache, Linux metadata, quotas and defragmentation.',
    1)
data = data.replace(
    'prevents the allocation map from regressing into textual inclusion,',
    'prevents extracted allocation/index/resize components from regressing into textual inclusion,',
    1)
readme.write_text(data)

# Negative regression strings in the maintainability policy are intentional;
# no other live source/manifests may retain the old include filename.
stale = []
for path in root.rglob('*'):
    if not path.is_file() or '.git' in path.parts or path in {
        Path('tools/kernel-index-object-refactor.py'),
        Path('tests/native-kernel-maintainability-policy.sh'),
    }:
        continue
    try:
        data = path.read_text()
    except UnicodeDecodeError:
        continue
    if 'infiltratorfs_index_tree.inc' in data:
        stale.append(str(path))
assert not stale, stale
