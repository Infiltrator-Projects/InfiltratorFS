#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import re

root = Path('.')
kernel = root / 'kernel'
old_core = kernel / 'infiltratorfs.c'
new_core = kernel / 'infiltratorfs_core.c'
old_alloc = kernel / 'infiltratorfs_allocation_map.inc'
new_alloc = kernel / 'infiltratorfs_allocation_map.c'
internal = kernel / 'infiltratorfs_internal.h'

assert old_core.exists(), 'expected pre-refactor kernel/infiltratorfs.c'
assert old_alloc.exists(), 'expected pre-refactor allocation map include'
assert not new_core.exists()
assert not new_alloc.exists()
assert not internal.exists()

header = '''// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATORFS_INTERNAL_H
#define INFILTRATORFS_INTERNAL_H

#include <linux/atomic.h>
#include <linux/blkdev.h>
#include <linux/bitops.h>
#include <linux/buffer_head.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/delay.h>
#include <linux/dirent.h>
#include <linux/falloc.h>
#include <linux/fiemap.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/lz4.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pagevec.h>
#include <linux/pagemap.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/uidgid.h>
#include <linux/uio.h>
#include <linux/user_namespace.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/writeback.h>
#include <linux/xattr.h>

#include "infiltratorfs_format.h"
#include "infiltratorfs_ioctl.h"
#include "iac1.h"

#define INFILTRATORFS_NAME "infiltratorfs"
#define INFILTRATORFS_MAGIC 0x494e4653u
#define INFILFS_ALLOCATION_RESERVATION_SHARDS 64u
#define INFILFS_LINUX_META_DIRECTORY ".infilfs-posix-meta"

struct infilfs_parallel_reservation {
    u64 start;
    u64 count;
    u32 shard;
    bool active;
};

enum infilfs_data_workload {
    INFILFS_DATA_WORKLOAD_SEQUENTIAL = 0,
    INFILFS_DATA_WORKLOAD_RANDOM,
    INFILFS_DATA_WORKLOAD_SPARSE,
};

enum infilfs_media_profile {
    INFILFS_MEDIA_BALANCED = 0,
    INFILFS_MEDIA_ROTATIONAL,
    INFILFS_MEDIA_NONROTATIONAL,
};

enum infilfs_media_override {
    INFILFS_MEDIA_OVERRIDE_AUTO = 0,
    INFILFS_MEDIA_OVERRIDE_BALANCED,
    INFILFS_MEDIA_OVERRIDE_ROTATIONAL,
    INFILFS_MEDIA_OVERRIDE_NONROTATIONAL,
};

struct infilfs_fs_context {
    enum infilfs_media_override media_override;
};

struct infilfs_quota_rule;
struct infilfs_project_root;

struct infilfs_sb_info {
    struct infilfs_superblock_disk disk;
    u64 device_blocks;
    struct mutex write_lock;
    struct mutex linux_meta_lock;
    struct mutex resize_lock;
    bool resize_active;
    struct mutex quota_lock;
    struct infilfs_quota_rule *quota_rules;
    size_t quota_rule_count;
    struct infilfs_project_root *project_roots;
    size_t project_root_count;
    rwlock_t bitmap_lock;
    u8 *bitmap;
    size_t bitmap_bytes;
    u64 *allocation_leaf_blocks;
    u64 *allocation_branch_blocks;
    size_t allocation_leaf_count;
    size_t allocation_branch_count;
    size_t allocation_level1_count;
    size_t allocation_level2_count;
    const u8 *visible_bitmap;
    size_t visible_bitmap_bytes;
    const u8 *validation_bitmap;
    size_t validation_bitmap_bytes;
    u8 *snapshot_bitmap;
    u64 data_alloc_hint;
    u64 metadata_alloc_hint;
    spinlock_t allocation_reservation_locks[
        INFILFS_ALLOCATION_RESERVATION_SHARDS];
    unsigned long *allocation_reservations;
    size_t allocation_reservation_bytes;
    u64 allocation_reservation_hints[
        INFILFS_ALLOCATION_RESERVATION_SHARDS];
    atomic64_t allocation_reservation_steer;
    atomic64_t allocation_reserved_blocks;
    atomic64_t allocation_active_reservations;
    atomic64_t allocation_peak_active_reservations;
    atomic64_t allocation_reservation_successes;
    atomic64_t allocation_reservation_conflicts;
    atomic64_t allocation_workload_sequential;
    atomic64_t allocation_workload_random;
    atomic64_t allocation_workload_sparse;
    atomic64_t allocation_locality_scored;
    atomic64_t allocation_best_fit;
    atomic64_t allocation_media_rotational_scored;
    atomic64_t allocation_media_nonrotational_scored;
    atomic64_t allocation_media_balanced_scored;
    enum infilfs_media_profile media_profile;
    bool media_profile_overridden;
    bool rw_enabled;
    bool write_poisoned;
    bool checkpoint_repair_needed;
};

struct infilfs_inode_info {
    u64 object_block;
    u64 data_allocation_hint;
    u64 portable_flags;
    u16 object_type;
    u8 object_id[16];
    char *symlink_target;
};

struct infilfs_dir_lookup {
    const char *name;
    size_t name_len;
    u8 object_id[16];
    u16 object_type;
    bool found;
};

struct infilfs_dir_emit_state {
    struct dir_context *ctx;
    struct inode *dir;
    bool has_linux_meta;
    bool hide_linux_meta;
    u64 index;
};

struct infilfs_dir_snapshot_entry {
    struct list_head link;
    u8 object_id[16];
    u16 object_type;
    u16 name_len;
    u8 name[];
};

struct infilfs_dir_snapshot {
    struct list_head entries;
};

struct infilfs_allocation_layout {
    u64 *leaf_blocks;
    u64 *branch_blocks;
    size_t leaf_count;
    size_t branch_count;
    size_t level1_count;
    size_t level2_count;
};

static inline struct infilfs_sb_info *INFILFS_SB(struct super_block *sb)
{
    return sb->s_fs_info;
}

static inline u64 infilfs_volume_blocks(const struct infilfs_sb_info *sbi)
{
    return sbi ? le64_to_cpu(sbi->disk.total_blocks) : 0;
}

bool infilfs_crc64_block_valid(
    const u8 block[INFILFS_DISK_BLOCK_SIZE],
    size_t checksum_offset, size_t checksum_size);
int infilfs_read_block(struct super_block *sb, u64 block, void *out);

void infilfs_allocation_layout_destroy(struct infilfs_allocation_layout *layout);
void infilfs_allocation_cache_destroy(struct infilfs_sb_info *sbi);
void infilfs_allocation_cache_replace(
    struct infilfs_sb_info *sbi, struct infilfs_allocation_layout *layout);
int infilfs_allocation_cache_view(
    struct infilfs_sb_info *sbi,
    const struct infilfs_superblock_disk *disk,
    struct infilfs_allocation_layout *layout);
int infilfs_allocation_counts(
    u64 total, size_t *leaves_out, size_t *level1_out,
    size_t *level2_out, size_t *branches_out);
int infilfs_allocation_runtime_bytes(u64 total, size_t *bytes_out);
int infilfs_allocation_map_load(
    struct super_block *sb, const struct infilfs_superblock_disk *disk,
    u8 **bitmap_out, size_t *bitmap_bytes_out,
    struct infilfs_allocation_layout *layout_out);

#endif /* INFILTRATORFS_INTERNAL_H */
'''
internal.write_text(header)

core = old_core.read_text()
split_marker = 'static int infilfs_linux_meta_get_special('
split = core.find(split_marker)
assert split > 0, 'could not find core declaration boundary'
retained = core[split:]
magic = '''// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"

static const u8 infilfs_disk_magic[8] = {
    'I', 'N', 'F', 'S', '2', '0', '2', '6'
};
static const u8 infilfs_object_magic[8] = {
    'I', 'N', 'F', 'O', 'B', 'J', '0', '1'
};
static const u8 infilfs_directory_page_magic[8] = {
    'I', 'N', 'F', 'S', 'D', 'P', '0', '1'
};
static const u8 infilfs_directory_branch_page_magic[8] = {
    'I', 'N', 'F', 'S', 'D', 'B', '0', '1'
};
static const u8 infilfs_index_page_magic[8] = {
    'I', 'N', 'F', 'S', 'I', 'P', '0', '1'
};
static const u8 infilfs_index_branch_page_magic[8] = {
    'I', 'N', 'F', 'S', 'I', 'B', '0', '1'
};
static const u8 infilfs_extent_page_magic[8] = {
    'I', 'N', 'F', 'S', 'E', 'P', '0', '1'
};

'''
core = magic + retained

core, n = re.subn(
    r'static struct infilfs_sb_info \*INFILFS_SB\(struct super_block \*sb\)\n\{\n    return sb->s_fs_info;\n\}\n\n',
    '', core, count=1)
assert n == 1, 'INFILFS_SB helper removal failed'
core, n = re.subn(
    r'/\*\n \* device_blocks is the physical backing-store bound\..*?\nstatic u64 infilfs_volume_blocks\(const struct infilfs_sb_info \*sbi\)\n\{\n    return sbi \? le64_to_cpu\(sbi->disk.total_blocks\) : 0;\n\}\n\n',
    '', core, count=1, flags=re.S)
assert n == 1, 'volume-block helper removal failed'

assert 'static bool infilfs_crc64_block_valid(' in core
core = core.replace('static bool infilfs_crc64_block_valid(',
                    'bool infilfs_crc64_block_valid(', 1)
assert 'static int infilfs_read_block(struct super_block *sb, u64 block, void *out)' in core
core = core.replace(
    'static int infilfs_read_block(struct super_block *sb, u64 block, void *out)',
    'int infilfs_read_block(struct super_block *sb, u64 block, void *out)', 1)
assert '#include "infiltratorfs_allocation_map.inc"' in core
core = core.replace('#include "infiltratorfs_allocation_map.inc"\n', '', 1)
new_core.write_text(core)
old_core.unlink()

alloc = old_alloc.read_text()
alloc = alloc.replace('// SPDX-License-Identifier: GPL-3.0-or-later\n',
                      '// SPDX-License-Identifier: GPL-3.0-or-later\n#include "infiltratorfs_internal.h"\n', 1)
alloc, n = re.subn(r'\nstruct infilfs_allocation_layout \{.*?\n\};\n',
                   '\n', alloc, count=1, flags=re.S)
assert n == 1, 'allocation-layout type extraction failed'
alloc, n = re.subn(
    r'\nstatic int infilfs_allocation_counts\(\n    u64 total, size_t \*leaves_out, size_t \*level1_out,\n    size_t \*level2_out, size_t \*branches_out\);\n',
    '\n', alloc, count=1)
assert n == 1, 'allocation-count forward declaration removal failed'
exports = {
    'static void infilfs_allocation_layout_destroy(': 'void infilfs_allocation_layout_destroy(',
    'static void infilfs_allocation_cache_destroy(': 'void infilfs_allocation_cache_destroy(',
    'static void infilfs_allocation_cache_replace(': 'void infilfs_allocation_cache_replace(',
    'static int infilfs_allocation_cache_view(': 'int infilfs_allocation_cache_view(',
    'static int infilfs_allocation_counts(': 'int infilfs_allocation_counts(',
    'static int infilfs_allocation_runtime_bytes(': 'int infilfs_allocation_runtime_bytes(',
    'static int infilfs_allocation_map_load(': 'int infilfs_allocation_map_load(',
}
for old, new in exports.items():
    assert old in alloc, f'missing allocation export {old}'
    alloc = alloc.replace(old, new, 1)
new_alloc.write_text(alloc)
old_alloc.unlink()

text_suffixes = {'.c', '.h', '.inc', '.sh', '.yml', '.yaml', '.md', '.txt', '.in'}
skip = {new_core, new_alloc, internal, Path('tools/kernel-multiobject-refactor.py'), Path('.github/workflows/kernel-multiobject-refactor.yml')}
for path in root.rglob('*'):
    if not path.is_file() or path in skip or path.suffix not in text_suffixes:
        continue
    try:
        data = path.read_text()
    except UnicodeDecodeError:
        continue
    changed = data.replace('kernel/infiltratorfs.c', 'kernel/infiltratorfs_core.c')
    changed = changed.replace('infiltratorfs_allocation_map.inc', 'infiltratorfs_allocation_map.c')
    if path in {
        Path('packaging/build-linux-packages.sh'),
        Path('tests/native-complete-qualification.sh'),
        Path('.github/workflows/kernel-module.yml'),
    }:
        changed = changed.replace('infiltratorfs.c', 'infiltratorfs_core.c')
    if changed != data:
        path.write_text(changed)

for name in [
    'packaging/build-linux-packages.sh',
    'tests/native-complete-qualification.sh',
    '.github/workflows/kernel-module.yml',
]:
    path = Path(name)
    data = path.read_text()
    if 'infiltratorfs_internal.h' not in data:
        data = data.replace(
            'infiltratorfs_core.c infiltratorfs_format.h',
            'infiltratorfs_core.c infiltratorfs_internal.h infiltratorfs_format.h')
        data = data.replace(
            'kernel/infiltratorfs_core.c \\\n             kernel/infiltratorfs_format.h',
            'kernel/infiltratorfs_core.c \\\n             kernel/infiltratorfs_internal.h \\\n             kernel/infiltratorfs_format.h')
    assert 'infiltratorfs_internal.h' in data, f'could not add internal header to {name}'
    path.write_text(data)

package = Path('packaging/build-linux-packages.sh')
data = package.read_text()
needle = '"usr/src/infiltratorfs-${package_version}/infiltratorfs_core.c$" \\\n'
if 'usr/src/infiltratorfs-${package_version}/infiltratorfs_internal.h$' not in data:
    assert needle in data
    data = data.replace(
        needle,
        needle + '    "usr/src/infiltratorfs-${package_version}/infiltratorfs_internal.h$" \\\n',
        1)
package.write_text(data)

makefile = kernel / 'Makefile'
data = makefile.read_text()
data = data.replace(
    '# This is the implementation-level contract for the single translation-unit\n# Linux driver.',
    '# This is the implementation-level contract for the composite\n# Linux driver.')
data = data.replace(
    '#   infiltratorfs.c is the only top-level module translation unit.\n#   infiltratorfs_rw.inc is the only nested implementation compositor; leaf\n#   .inc files must not start including each other and creating hidden cycles.',
    '#   infiltratorfs.ko is a composite Kbuild module.\n#   infiltratorfs_core.c owns VFS/mount/checkpoint orchestration.\n#   infiltratorfs_allocation_map.c owns Format 0.17 allocation-map loading.\n#   infiltratorfs_rw.inc is the only remaining nested implementation compositor;\n#   leaf .inc files must not start including each other or creating hidden cycles.')
assert 'obj-m += infiltratorfs.o\n' in data
data = data.replace('obj-m += infiltratorfs.o\n',
                    'obj-m += infiltratorfs.o\ninfiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o\n',
                    1)
makefile.write_text(data)

policy = Path('tests/native-kernel-maintainability-policy.sh')
data = policy.read_text()
data = data.replace('driver="$kernel/infiltratorfs.c"',
                    'driver="$kernel/infiltratorfs_core.c"')
data = data.replace(
    '# Only the top-level driver and the explicit RW compositor may textually compose',
    '# Only the core object and the explicit RW compositor may textually compose')
data = data.replace(
    '# implementation .inc units. A leaf .inc importing another leaf creates hidden',
    '# remaining implementation .inc units. A leaf .inc importing another leaf creates hidden')
insert = '''
# The native driver must stay a genuine multi-object Kbuild module.  The
# allocation map is the first extracted subsystem and must never regress into
# textual inclusion.
grep -Fq 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o' "$makefile" || \
    fail 'kernel module is no longer built from explicit component objects'
test -f "$kernel/infiltratorfs_internal.h" || fail 'missing private kernel API header'
test -f "$kernel/infiltratorfs_allocation_map.c" || fail 'allocation map object missing'
test ! -e "$kernel/infiltratorfs_allocation_map.inc" || fail 'allocation map regressed to textual include'
! grep -Fq 'infiltratorfs_allocation_map.inc' "$driver" || fail 'core textually includes allocation map'

'''
anchor = '# Only the core object and the explicit RW compositor may textually compose'
assert anchor in data
data = data.replace(anchor, insert + anchor, 1)
policy.write_text(data)

allocation_policy = Path('tests/allocation-tree-format-policy.sh')
assert 'kernel/infiltratorfs_allocation_map.c' in allocation_policy.read_text()

stale = []
for path in root.rglob('*'):
    if not path.is_file() or '.git' in path.parts or path in {Path('tools/kernel-multiobject-refactor.py'), Path('.github/workflows/kernel-multiobject-refactor.yml')}:
        continue
    try:
        data = path.read_text()
    except UnicodeDecodeError:
        continue
    if 'kernel/infiltratorfs.c' in data or 'infiltratorfs_allocation_map.inc' in data:
        stale.append(str(path))
assert not stale, f'stale kernel composition references: {stale}'
