#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path

root = Path('.')
kernel = root / 'kernel'
core = kernel / 'infiltratorfs_core.c'
internal = kernel / 'infiltratorfs_internal.h'
old_resize = kernel / 'infiltratorfs_resize.inc'
new_resize = kernel / 'infiltratorfs_resize.c'
makefile = kernel / 'Makefile'

assert core.exists() and internal.exists() and old_resize.exists()
assert not new_resize.exists()

# Turn the already-qualified resize unit into a normal translation unit.
resize = old_resize.read_text()
resize = resize.replace('// SPDX-License-Identifier: GPL-3.0-or-later\n',
                        '// SPDX-License-Identifier: GPL-3.0-or-later\n#include "infiltratorfs_internal.h"\n', 1)
assert 'static int infilfs_native_resize_volume(' in resize
resize = resize.replace('static int infilfs_native_resize_volume(',
                        'int infilfs_native_resize_volume(', 1)
new_resize.write_text(resize)
old_resize.unlink()

text = core.read_text()
assert '#include "infiltratorfs_resize.inc"\n' in text
core.write_text(text.replace('#include "infiltratorfs_resize.inc"\n', '', 1))

# The resize object calls a deliberately small set of core/RW services. Make
# those internal-module APIs explicit rather than relying on textual inclusion.
providers = {
    kernel / 'infiltratorfs_rw_legacy.inc': [
        ('static bool infilfs_rw_bitmap_get(', 'bool infilfs_rw_bitmap_get('),
        ('static void infilfs_rw_bitmap_set(', 'void infilfs_rw_bitmap_set('),
        ('static int infilfs_rw_snapshot_count(', 'int infilfs_rw_snapshot_count('),
        ('static int infilfs_rw_encode_superblock(', 'int infilfs_rw_encode_superblock('),
        ('static int infilfs_rw_write_block(', 'int infilfs_rw_write_block('),
    ],
    kernel / 'infiltratorfs_allocation_publish.inc': [
        ('static int infilfs_rw_allocation_write_page(', 'int infilfs_rw_allocation_write_page('),
    ],
    kernel / 'infiltratorfs_parallel_alloc.inc': [
        ('static void infilfs_parallel_shard_bounds(', 'void infilfs_parallel_shard_bounds('),
    ],
    kernel / 'infiltratorfs_rw_data.inc': [
        ('static int infilfs_native_pending_flush_sb(', 'int infilfs_native_pending_flush_sb('),
    ],
}
for path, replacements in providers.items():
    data = path.read_text()
    for old, new in replacements:
        assert old in data, f'{old} missing from {path}'
        data = data.replace(old, new, 1)
    path.write_text(data)

header = internal.read_text()
anchor = 'int infilfs_allocation_map_load(\n    struct super_block *sb, const struct infilfs_superblock_disk *disk,\n    u8 **bitmap_out, size_t *bitmap_bytes_out,\n    struct infilfs_allocation_layout *layout_out);\n'
assert anchor in header
api = '''\n/* Services shared with the independently compiled resize component. */\nbool infilfs_rw_bitmap_get(const u8 *bitmap, u64 block);\nvoid infilfs_rw_bitmap_set(u8 *bitmap, u64 block, bool allocated);\nint infilfs_rw_snapshot_count(struct super_block *sb, u32 *count_out);\nint infilfs_rw_encode_superblock(\n    const struct infilfs_superblock_disk *disk,\n    u8 block[INFILFS_DISK_BLOCK_SIZE]);\nint infilfs_rw_write_block(struct super_block *sb, u64 block, const void *data);\nint infilfs_rw_allocation_write_page(\n    struct super_block *sb, u64 physical, const u8 magic[8],\n    u64 generation, u64 logical, u32 level, u32 entries,\n    const void *payload, u32 bytes);\nvoid infilfs_parallel_shard_bounds(\n    const struct infilfs_sb_info *sbi, u32 shard, u64 *start, u64 *end);\nint infilfs_native_pending_flush_sb(struct super_block *sb);\nint infilfs_native_resize_volume(\n    struct super_block *sb, struct infilfs_resize_request *request);\n'''
internal.write_text(header.replace(anchor, anchor + api, 1))

mk = makefile.read_text()
old = 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o'
new = 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o infiltratorfs_resize.o'
assert old in mk
mk = mk.replace(old, new, 1)
mk = mk.replace(
    '#   infiltratorfs_allocation_map.c owns Format 0.17 allocation-map loading.\n',
    '#   infiltratorfs_allocation_map.c owns Format 0.17 allocation-map loading.\n#   infiltratorfs_resize.c owns mounted geometry transitions.\n', 1)
makefile.write_text(mk)

# Update source manifests and source-inspection tests to the normal C unit.
for path in root.rglob('*'):
    if not path.is_file() or '.git' in path.parts or path == Path('tools/kernel-resize-object-refactor.py'):
        continue
    if path.suffix not in {'.sh', '.yml', '.yaml', '.md', '.txt', '.in', '.c', '.h'}:
        continue
    try:
        data = path.read_text()
    except UnicodeDecodeError:
        continue
    changed = data.replace('infiltratorfs_resize.inc', 'infiltratorfs_resize.c')
    if changed != data:
        path.write_text(changed)

# Strengthen the maintainability policy: resize is now a required compiled
# component, not permitted to regress into the compositor.
policy = Path('tests/native-kernel-maintainability-policy.sh')
data = policy.read_text()
data = data.replace(
    "grep -Fq 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o' \"$makefile\" || \\\n    fail 'kernel module is no longer built from explicit component objects'",
    "grep -Fq 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o infiltratorfs_resize.o' \"$makefile\" || \\\n    fail 'kernel module is no longer built from explicit component objects'",
    1)
marker = "test -f \"$kernel/infiltratorfs_allocation_map.c\" || fail 'allocation map object missing'\n"
assert marker in data
data = data.replace(marker, marker +
    "test -f \"$kernel/infiltratorfs_resize.c\" || fail 'resize object missing'\n"
    "test ! -e \"$kernel/infiltratorfs_resize.inc\" || fail 'resize regressed to textual include'\n"
    "! grep -Fq 'infiltratorfs_resize.inc' \"$driver\" || fail 'core textually includes resize'\n", 1)
policy.write_text(data)

# The kernel-local README tracks the actual component boundary, not a filename
# list for every remaining migration include.
readme = Path('kernel/README.md')
data = readme.read_text()
data = data.replace(
    '`infiltratorfs_core.c` owns VFS, mount and checkpoint orchestration, while `infiltratorfs_allocation_map.c` is the first separately compiled subsystem and owns Format 0.17 allocation-map loading.',
    '`infiltratorfs_core.c` owns VFS, mount and checkpoint orchestration, `infiltratorfs_allocation_map.c` owns Format 0.17 allocation-map loading, and `infiltratorfs_resize.c` owns mounted geometry transitions.',
    1)
readme.write_text(data)

# No stale live dependency on the old resize include may remain outside this
# one-shot helper itself.
stale = []
for path in root.rglob('*'):
    if not path.is_file() or '.git' in path.parts or path == Path('tools/kernel-resize-object-refactor.py'):
        continue
    try:
        data = path.read_text()
    except UnicodeDecodeError:
        continue
    if 'infiltratorfs_resize.inc' in data:
        stale.append(str(path))
assert not stale, stale
