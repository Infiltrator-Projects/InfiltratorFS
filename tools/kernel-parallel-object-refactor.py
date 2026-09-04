#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path

root = Path('.')
kernel = root / 'kernel'
core = kernel / 'infiltratorfs_core.c'
internal = kernel / 'infiltratorfs_internal.h'
rw = kernel / 'infiltratorfs_rw.inc'
legacy = kernel / 'infiltratorfs_rw_legacy.inc'
data_path = kernel / 'infiltratorfs_rw_data.inc'
old_parallel = kernel / 'infiltratorfs_parallel_alloc.inc'
new_parallel = kernel / 'infiltratorfs_parallel_alloc.c'
makefile = kernel / 'Makefile'
maint = Path('tests/native-kernel-maintainability-policy.sh')
parallel_policy = Path('tests/native-parallel-allocation-policy.sh')
readme = kernel / 'README.md'

assert old_parallel.exists() and not new_parallel.exists()

# Move the transaction state that was already effectively shared by textual
# composition into the module-private header. This is not a public ABI.
legacy_text = legacy.read_text()
tx_block = '''#define INFILFS_RW_FREE_RANGES_INITIAL 32u
#define INFILFS_RW_ALLOC_RANGES_INITIAL 64u

struct infilfs_rw_free_range {
    u64 start;
    u64 count;
};

struct infilfs_rw_tx {
    struct super_block *sb;
    struct infilfs_sb_info *sbi;
    struct infilfs_superblock_disk next_sb;
    u8 *bitmap;
    size_t bitmap_bytes;
    u64 generation;
    u64 free_blocks;
    struct infilfs_rw_free_range *deferred;
    size_t deferred_count;
    size_t deferred_capacity;
    /*
     * Allocation journal for operation-level rollback. Native transactions
     * only mark free blocks allocated during an operation; frees remain
     * deferred until publication. Recording those new runs is sufficient to
     * undo a failed syscall without cloning the whole allocation bitmap.
     */
    struct infilfs_rw_free_range *allocated;
    size_t allocated_count;
    size_t allocated_capacity;

    /*
     * Rebuildable transaction-local index of maximal free runs. The live
     * transaction bitmap remains authoritative; this cache only avoids
     * rescanning the whole device for every allocation.
     */
    struct infilfs_rw_free_range *free_extents;
    size_t free_extent_count;
    size_t free_extent_capacity;
    bool free_extent_index_valid;
};

'''
assert legacy_text.count(tx_block) == 1
legacy_text = legacy_text.replace(tx_block, '', 1)

# These two helpers form the narrow allocator -> transaction-index boundary.
for old, new in [
    ('static void infilfs_rw_free_extent_index_invalidate(',
     'void infilfs_rw_free_extent_index_invalidate('),
    ('static int infilfs_rw_free_extent_index_remove(',
     'int infilfs_rw_free_extent_index_remove('),
]:
    assert legacy_text.count(old) == 1, old
    legacy_text = legacy_text.replace(old, new, 1)
legacy.write_text(legacy_text)

header = internal.read_text()
anchor = '''struct infilfs_parallel_reservation {
    u64 start;
    u64 count;
    u32 shard;
    bool active;
};
'''
assert anchor in header
header = header.replace(anchor, anchor + '''
#define INFILFS_RW_FREE_RANGES_INITIAL 32u
#define INFILFS_RW_ALLOC_RANGES_INITIAL 64u

/* Private transaction/allocation state shared by compiled native components. */
struct infilfs_rw_free_range {
    u64 start;
    u64 count;
};

struct infilfs_rw_tx {
    struct super_block *sb;
    struct infilfs_sb_info *sbi;
    struct infilfs_superblock_disk next_sb;
    u8 *bitmap;
    size_t bitmap_bytes;
    u64 generation;
    u64 free_blocks;
    struct infilfs_rw_free_range *deferred;
    size_t deferred_count;
    size_t deferred_capacity;
    struct infilfs_rw_free_range *allocated;
    size_t allocated_count;
    size_t allocated_capacity;
    struct infilfs_rw_free_range *free_extents;
    size_t free_extent_count;
    size_t free_extent_capacity;
    bool free_extent_index_valid;
};
''', 1)

api_anchor = '''int infilfs_native_resize_volume(
    struct super_block *sb, struct infilfs_resize_request *request);
'''
assert api_anchor in header
parallel_api = '''
/* Services shared with the compiled volatile parallel allocator. */
const char *infilfs_media_profile_name(enum infilfs_media_profile profile);
void infilfs_rw_free_extent_index_invalidate(struct infilfs_rw_tx *tx);
int infilfs_rw_free_extent_index_remove(
    struct infilfs_rw_tx *tx, u64 start, u64 count);
u64 infilfs_native_metadata_reserve_blocks(const struct infilfs_sb_info *sbi);
u64 infilfs_native_visible_free_blocks(struct super_block *sb);
int infilfs_parallel_allocator_mount_init(struct super_block *sb);
void infilfs_parallel_allocator_mount_destroy(struct super_block *sb);
int infilfs_parallel_tx_claim(
    struct infilfs_rw_tx *tx, u64 start, u64 count, bool consume_reservation);
bool infilfs_parallel_range_reserved(
    const struct infilfs_sb_info *sbi, u64 start, u64 count);
u64 infilfs_parallel_object_preferred(
    const struct infilfs_sb_info *sbi, const u8 object_id[16]);
void infilfs_parallel_note_workload(
    struct infilfs_sb_info *sbi, enum infilfs_data_workload workload);
int infilfs_parallel_reserve_data(
    struct super_block *sb, u64 count, u64 preferred,
    struct infilfs_parallel_reservation *reservation);
void infilfs_parallel_release_reservation(
    struct super_block *sb, struct infilfs_parallel_reservation *reservation);
int infilfs_parallel_consume_reservation(
    struct infilfs_rw_tx *tx, struct infilfs_parallel_reservation *reservation,
    u64 count, u64 *start_out);
'''
header = header.replace(api_anchor, api_anchor + parallel_api, 1)
internal.write_text(header)

# Core/media helper becomes a module-private service, not a static same-TU helper.
core_text = core.read_text()
old = 'static const char *infilfs_media_profile_name('
assert core_text.count(old) == 1
core.write_text(core_text.replace(old, 'const char *infilfs_media_profile_name(', 1))

# RW-data capacity helpers are consumed by the separately compiled reservation
# object. Their semantics remain unchanged.
data_text = data_path.read_text()
for old, new in [
    ('static u64 infilfs_native_metadata_reserve_blocks(',
     'u64 infilfs_native_metadata_reserve_blocks('),
    ('static u64 infilfs_native_visible_free_blocks(',
     'u64 infilfs_native_visible_free_blocks('),
]:
    assert data_text.count(old) == 1, old
    data_text = data_text.replace(old, new, 1)
data_path.write_text(data_text)

# Turn the reservation implementation into a normal object and expose only the
# functions with existing external callers in the native module.
p = old_parallel.read_text()
p = p.replace('// SPDX-License-Identifier: GPL-3.0-or-later\n',
              '// SPDX-License-Identifier: GPL-3.0-or-later\n#include "infiltratorfs_internal.h"\n', 1)
for name, rettype in [
    ('infilfs_parallel_shard_bounds', 'void'),
    ('infilfs_parallel_object_preferred', 'u64'),
    ('infilfs_parallel_range_reserved', 'bool'),
    ('infilfs_parallel_allocator_mount_init', 'int'),
    ('infilfs_parallel_allocator_mount_destroy', 'void'),
    ('infilfs_parallel_tx_claim', 'int'),
    ('infilfs_parallel_note_workload', 'void'),
    ('infilfs_parallel_reserve_data', 'int'),
    ('infilfs_parallel_release_reservation', 'void'),
    ('infilfs_parallel_consume_reservation', 'int'),
]:
    old_sig = f'static {rettype} {name}('
    if old_sig in p:
        p = p.replace(old_sig, f'{rettype} {name}(', 1)
    else:
        # shard_bounds was already exposed for resize.
        exposed = f'{rettype} {name}('
        assert exposed in p, name
new_parallel.write_text(p)
old_parallel.unlink()

# The RW compositor no longer owns the reservation implementation or its local
# forward declarations. Header declarations now express the dependency.
rw_text = rw.read_text()
for decl in [
'''static int infilfs_parallel_allocator_mount_init(struct super_block *sb);
''',
'''static void infilfs_parallel_allocator_mount_destroy(struct super_block *sb);
''',
'''static int infilfs_parallel_tx_claim(struct infilfs_rw_tx *tx, u64 start,
                                     u64 count, bool consume_reservation);
''',
'''static bool infilfs_parallel_range_reserved(
    const struct infilfs_sb_info *sbi, u64 start, u64 count);
''',
'''static u64 infilfs_native_metadata_reserve_blocks(
    const struct infilfs_sb_info *sbi);
''',
'''static u64 infilfs_native_visible_free_blocks(struct super_block *sb);
''',
]:
    assert decl in rw_text, decl
    rw_text = rw_text.replace(decl, '', 1)
assert '#include "infiltratorfs_parallel_alloc.inc"\n' in rw_text
rw_text = rw_text.replace('#include "infiltratorfs_parallel_alloc.inc"\n', '', 1)
rw.write_text(rw_text)

# Kbuild now links five explicit module-private objects.
mk = makefile.read_text()
old_line = ('infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o '
            'infiltratorfs_resize.o infiltratorfs_index_tree.o')
new_line = old_line + ' infiltratorfs_parallel_alloc.o'
assert old_line in mk
mk = mk.replace(old_line, new_line, 1)
marker = '#   infiltratorfs_index_tree.c owns Format 0.17 object-index reads/snapshots.\n'
assert marker in mk
mk = mk.replace(marker, marker +
    '#   infiltratorfs_parallel_alloc.c owns volatile sharded data reservations.\n', 1)
makefile.write_text(mk)

# Source packaging, DKMS and workflow copy lists must follow the compiled unit.
for path in root.rglob('*'):
    if not path.is_file() or '.git' in path.parts or path == Path('tools/kernel-parallel-object-refactor.py'):
        continue
    if path.suffix not in {'.sh', '.yml', '.yaml', '.md', '.txt', '.in', '.c', '.h'}:
        continue
    try:
        text = path.read_text()
    except UnicodeDecodeError:
        continue
    changed = text.replace('infiltratorfs_parallel_alloc.inc', 'infiltratorfs_parallel_alloc.c')
    if changed != text:
        path.write_text(changed)

# Update the allocator-specific policy to assert the new compiled boundary.
pp = parallel_policy.read_text()
pp = pp.replace('allocator="$root/kernel/infiltratorfs_parallel_alloc.inc"',
                'allocator="$root/kernel/infiltratorfs_parallel_alloc.c"', 1)
pp = pp.replace("grep -Fq '#include \"infiltratorfs_parallel_alloc.c\"' \"$rw\"",
                "! grep -Fq '#include \"infiltratorfs_parallel_alloc.c\"' \"$rw\"", 1)
pp = pp.replace("grep -Fq 'infiltratorfs_parallel_alloc.c' \"$package\"",
                "grep -Fq 'infiltratorfs_parallel_alloc.c' \"$package\"", 1)
parallel_policy.write_text(pp)

# Maintainability policy: exact 5-object Kbuild, no textual allocator include,
# and remove it from the remaining compositor order contract.
mp = maint.read_text()
old_guard = "grep -Fqx 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o infiltratorfs_resize.o infiltratorfs_index_tree.o' \"$makefile\" || \\\n"
new_guard = "grep -Fqx 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o infiltratorfs_resize.o infiltratorfs_index_tree.o infiltratorfs_parallel_alloc.o' \"$makefile\" || \\\n"
assert old_guard in mp
mp = mp.replace(old_guard, new_guard, 1)
anchor = "test -f \"$kernel/infiltratorfs_index_tree.c\" || fail 'object-index tree object missing'\n"
assert anchor in mp
mp = mp.replace(anchor, anchor +
    "test -f \"$kernel/infiltratorfs_parallel_alloc.c\" || fail 'parallel allocator object missing'\n"
    "test ! -e \"$kernel/infiltratorfs_parallel_alloc.inc\" || fail 'parallel allocator regressed to textual include'\n"
    "! grep -Fq 'infiltratorfs_parallel_alloc.inc' \"$rw\" || fail 'RW compositor textually includes parallel allocator'\n", 1)
mp = mp.replace('    infiltratorfs_parallel_alloc.inc\n', '', 1)
maint.write_text(mp)

# Align local architecture prose with the newly compiled component.
doc = readme.read_text()
old = ('`infiltratorfs_resize.c` owns mounted geometry transitions, and '
       '`infiltratorfs_index_tree.c` owns Format 0.17 object-index reads and snapshots.')
new = ('`infiltratorfs_resize.c` owns mounted geometry transitions, '
       '`infiltratorfs_index_tree.c` owns Format 0.17 object-index reads and snapshots, and '
       '`infiltratorfs_parallel_alloc.c` owns volatile sharded data reservations.')
assert old in doc
doc = doc.replace(old, new, 1)
doc = doc.replace(
    'including allocation publication, parallel reservation, read/write data paths, namespace mutation, directory trees, page cache, Linux metadata, quotas and defragmentation.',
    'including allocation publication, read/write data paths, namespace mutation, directory trees, page cache, Linux metadata, quotas and defragmentation.',
    1)
doc = doc.replace(
    'prevents extracted allocation/index/resize components from regressing into textual inclusion,',
    'prevents extracted allocation/index/resize/reservation components from regressing into textual inclusion,',
    1)
readme.write_text(doc)

# Old filename may remain only as an intentional negative regression string in
# the maintainability policy and in this one-shot helper itself.
stale = []
for path in root.rglob('*'):
    if not path.is_file() or '.git' in path.parts or path in {
        Path('tools/kernel-parallel-object-refactor.py'),
        Path('tests/native-kernel-maintainability-policy.sh'),
    }:
        continue
    try:
        text = path.read_text()
    except UnicodeDecodeError:
        continue
    if 'infiltratorfs_parallel_alloc.inc' in text:
        stale.append(str(path))
assert not stale, stale
