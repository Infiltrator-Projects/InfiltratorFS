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
old_tree = kernel / 'infiltratorfs_directory_tree.inc'
new_tree = kernel / 'infiltratorfs_directory_tree.c'
makefile = kernel / 'Makefile'
maint = root / 'tests/native-kernel-maintainability-policy.sh'
readme = kernel / 'README.md'

assert old_tree.exists() and not new_tree.exists()

# The directory tree already shared deferred native transaction state with the
# data/namespace layers through textual composition. Move that state into the
# module-private header so compiled components have one canonical definition.
dt = data_path.read_text()

def take_before(text, start_marker, next_marker):
    start = text.index(start_marker)
    end = text.index(next_marker, start)
    return text[start:end], text[:start] + text[end:]

writer_macro = '#define INFILFS_NATIVE_WRITER_TAIL_SLOTS 64u\n'
assert dt.count(writer_macro) == 1
dt = dt.replace(writer_macro, '', 1)
undo_block, dt = take_before(dt, 'struct infilfs_native_undo {', 'struct infilfs_native_writer_tail {')
writer_block, dt = take_before(dt, 'struct infilfs_native_writer_tail {', 'struct infilfs_native_index_locator {')
index_locator_block, dt = take_before(dt, 'struct infilfs_native_index_locator {', 'struct infilfs_native_directory_locator {')
directory_locator_block, dt = take_before(dt, 'struct infilfs_native_directory_locator {', 'struct infilfs_native_pending {')
pending_block, dt = take_before(dt, 'struct infilfs_native_pending {', 'struct infilfs_native_checksum_node {')

# Data-path services consumed by the compiled tree get normal module-private
# linkage. Their behavior and ownership do not change.
for old, new in [
    ('static int infilfs_native_stage_block(', 'int infilfs_native_stage_block('),
    ('static int infilfs_native_store_private_or_cow(', 'int infilfs_native_store_private_or_cow('),
    ('static void infilfs_native_directory_locator_invalidate(', 'void infilfs_native_directory_locator_invalidate('),
]:
    assert dt.count(old) == 1, (old, dt.count(old))
    dt = dt.replace(old, new, 1)
data_path.write_text(dt)

# SHA-256 state is used by legacy/data/tree code. Make the type and helpers
# explicitly private-module shared instead of depending on include order.
lt = legacy.read_text()
sha_block, lt = take_before(lt, 'struct infilfs_rw_sha256_ctx {', 'bool infilfs_rw_bitmap_get(')
for old, new in [
    ('static void infilfs_rw_sha256_init(', 'void infilfs_rw_sha256_init('),
    ('static void infilfs_rw_sha256_update(', 'void infilfs_rw_sha256_update('),
    ('static void infilfs_rw_sha256_final(', 'void infilfs_rw_sha256_final('),
    ('static bool infilfs_rw_utf8_valid(', 'bool infilfs_rw_utf8_valid('),
    ('static void infilfs_rw_init_page(', 'void infilfs_rw_init_page('),
    ('static int infilfs_rw_finalize_page(', 'int infilfs_rw_finalize_page('),
    ('static int infilfs_rw_finalize_object(', 'int infilfs_rw_finalize_object('),
]:
    assert lt.count(old) == 1, (old, lt.count(old))
    lt = lt.replace(old, new, 1)
legacy.write_text(lt)

# Core directory validation constants/walker become private-module services.
ct = core.read_text()
for old, new in [
    ('static const u8 infilfs_directory_page_magic[8]', 'const u8 infilfs_directory_page_magic[8]'),
    ('static const u8 infilfs_directory_branch_page_magic[8]', 'const u8 infilfs_directory_branch_page_magic[8]'),
    ('static int infilfs_walk_dir_buffer(', 'int infilfs_walk_dir_buffer('),
]:
    assert ct.count(old) == 1, (old, ct.count(old))
    ct = ct.replace(old, new, 1)
old_proto = '''static int infilfs_tree_dir_lookup_name(
    struct inode *dir, const u8 *name, u16 name_len,
    struct infilfs_dir_lookup *search);
static int infilfs_tree_dir_for_each(
    struct inode *inode,
    int (*visitor)(const struct infilfs_dirent_disk *, const u8 *, void *),
    void *arg);

'''
assert old_proto in ct
ct = ct.replace(old_proto, '', 1)
old_utf8 = 'static bool infilfs_rw_utf8_valid(const u8 *s, size_t len);\n\n'
assert old_utf8 in ct
ct = ct.replace(old_utf8, '', 1)
core.write_text(ct)

# Build the independently compiled Format 0.17 directory-tree component.
tree = old_tree.read_text()
tree = tree.replace('// SPDX-License-Identifier: GPL-3.0-or-later\n',
                    '// SPDX-License-Identifier: GPL-3.0-or-later\n#include "infiltratorfs_internal.h"\n', 1)
for old, new in [
    ('static int infilfs_tree_dir_lookup_name(', 'int infilfs_tree_dir_lookup_name('),
    ('static int infilfs_tree_dir_for_each(', 'int infilfs_tree_dir_for_each('),
    ('static int __maybe_unused infilfs_native_tree_directory_update(', 'int infilfs_native_tree_directory_update('),
]:
    assert tree.count(old) == 1, (old, tree.count(old))
    tree = tree.replace(old, new, 1)
new_tree.write_text(tree)
old_tree.unlink()

# Private header owns the shared pending/SHA types and the narrow tree API.
h = internal.read_text()
if '#include <linux/workqueue.h>\n' not in h:
    anchor = '#include <linux/writeback.h>\n'
    assert anchor in h
    h = h.replace(anchor, anchor + '#include <linux/workqueue.h>\n', 1)
state_anchor = '''struct infilfs_rw_tx {
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
'''
assert state_anchor in h
shared_state = ('\n' + sha_block + writer_macro + undo_block + writer_block +
                index_locator_block + directory_locator_block + pending_block)
h = h.replace(state_anchor, state_anchor + shared_state, 1)
api_anchor = 'extern const struct address_space_operations infilfs_aops;\n'
assert api_anchor in h
api = '''
/* Services shared with the compiled Format 0.17 directory-tree layer. */
extern const u8 infilfs_directory_page_magic[8];
extern const u8 infilfs_directory_branch_page_magic[8];
void infilfs_rw_sha256_init(struct infilfs_rw_sha256_ctx *ctx);
void infilfs_rw_sha256_update(
    struct infilfs_rw_sha256_ctx *ctx, const u8 *data, size_t length);
void infilfs_rw_sha256_final(struct infilfs_rw_sha256_ctx *ctx, u8 out[32]);
bool infilfs_rw_utf8_valid(const u8 *s, size_t len);
void infilfs_rw_init_page(
    u8 *block, const u8 magic[8], const u8 owner[16], u64 generation);
int infilfs_rw_finalize_page(u8 block[INFILFS_DISK_BLOCK_SIZE]);
int infilfs_rw_finalize_object(u8 block[INFILFS_DISK_BLOCK_SIZE]);
int infilfs_walk_dir_buffer(
    const u8 *buffer, u32 bytes,
    int (*visitor)(const struct infilfs_dirent_disk *, const u8 *, void *),
    void *arg);
int infilfs_native_stage_block(struct super_block *sb, u64 block, const void *data);
int infilfs_native_store_private_or_cow(
    struct infilfs_native_pending *pending, u64 old_block,
    const u8 data[INFILFS_DISK_BLOCK_SIZE], u64 *new_block_out);
void infilfs_native_directory_locator_invalidate(struct infilfs_native_pending *pending);
int infilfs_tree_dir_lookup_name(
    struct inode *dir, const u8 *name, u16 name_len,
    struct infilfs_dir_lookup *search);
int infilfs_tree_dir_for_each(
    struct inode *inode,
    int (*visitor)(const struct infilfs_dirent_disk *, const u8 *, void *),
    void *arg);
int infilfs_native_tree_directory_update(
    struct infilfs_native_pending *pending, struct inode *dir,
    const struct qstr *remove_a, const struct qstr *remove_b,
    const struct qstr *add_name, const u8 add_id[16], u16 add_type,
    int link_delta, u64 *new_dir_block_out);
'''
h = h.replace(api_anchor, api_anchor + api, 1)
internal.write_text(h)

# Remove textual composition from the RW aggregator.
rwt = rw.read_text()
inc = '#include "infiltratorfs_directory_tree.inc"\n'
assert inc in rwt
rw.write_text(rwt.replace(inc, '', 1))

# Kbuild object nine and component contract.
mk = makefile.read_text()
old_line = ('infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o '
            'infiltratorfs_resize.o infiltratorfs_index_tree.o '
            'infiltratorfs_parallel_alloc.o infiltratorfs_allocation_publish.o '
            'infiltratorfs_read_cache.o infiltratorfs_pagecache.o')
new_line = old_line + ' infiltratorfs_directory_tree.o'
assert old_line in mk
mk = mk.replace(old_line, new_line, 1)
marker = '#   infiltratorfs_pagecache.c owns Linux folio/page-cache integration.\n'
assert marker in mk
mk = mk.replace(marker, marker +
    '#   infiltratorfs_directory_tree.c owns scalable Format 0.17 directory trees.\n', 1)
makefile.write_text(mk)

# Package/DKMS/source-copy manifests now ship the compiled source.
for path in root.rglob('*'):
    if not path.is_file() or '.git' in path.parts or path == Path('tools/kernel-directory-tree-object-refactor.py'):
        continue
    if path.suffix not in {'.sh', '.yml', '.yaml', '.md', '.txt', '.in', '.c', '.h'}:
        continue
    try:
        text = path.read_text()
    except UnicodeDecodeError:
        continue
    changed = text.replace('infiltratorfs_directory_tree.inc', 'infiltratorfs_directory_tree.c')
    if changed != text:
        path.write_text(changed)

# Maintainability: exact nine-object layout and no textual directory-tree
# regression. Remove the migrated .c name from the remaining include order.
mp = maint.read_text()
old_guard = "grep -Fqx 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o infiltratorfs_resize.o infiltratorfs_index_tree.o infiltratorfs_parallel_alloc.o infiltratorfs_allocation_publish.o infiltratorfs_read_cache.o infiltratorfs_pagecache.o' \"$makefile\" || \\\n"
new_guard = "grep -Fqx 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o infiltratorfs_resize.o infiltratorfs_index_tree.o infiltratorfs_parallel_alloc.o infiltratorfs_allocation_publish.o infiltratorfs_read_cache.o infiltratorfs_pagecache.o infiltratorfs_directory_tree.o' \"$makefile\" || \\\n"
assert old_guard in mp
mp = mp.replace(old_guard, new_guard, 1)
anchor = "test -f \"$kernel/infiltratorfs_pagecache.c\" || fail 'page-cache object missing'\n"
assert anchor in mp
mp = mp.replace(anchor, anchor +
    "test -f \"$kernel/infiltratorfs_directory_tree.c\" || fail 'directory-tree object missing'\n"
    "test ! -e \"$kernel/infiltratorfs_directory_tree.inc\" || fail 'directory tree regressed to textual include'\n"
    "! grep -Fq 'infiltratorfs_directory_tree.inc' \"$rw\" || fail 'RW compositor textually includes directory tree'\n", 1)
mp = mp.replace('    infiltratorfs_directory_tree.c\n', '', 1)
maint.write_text(mp)

# Local architecture prose follows the compiled boundary.
doc = readme.read_text()
old = ('`infiltratorfs_read_cache.c` owns verified read cursor/page caching, and '
       '`infiltratorfs_pagecache.c` owns Linux folio/page-cache integration.')
new = ('`infiltratorfs_read_cache.c` owns verified read cursor/page caching, '
       '`infiltratorfs_pagecache.c` owns Linux folio/page-cache integration, and '
       '`infiltratorfs_directory_tree.c` owns scalable Format 0.17 directory trees.')
assert old in doc
doc = doc.replace(old, new, 1)
doc = doc.replace(
    'including write-data paths, namespace mutation, directory trees, Linux metadata, quotas and defragmentation.',
    'including write-data paths, namespace mutation, Linux metadata, quotas and defragmentation.',
    1)
doc = doc.replace(
    'prevents extracted allocation/index/resize/reservation/publication/read-cache/page-cache components from regressing into textual inclusion,',
    'prevents extracted allocation/index/resize/reservation/publication/read-cache/page-cache/directory-tree components from regressing into textual inclusion,',
    1)
readme.write_text(doc)

# Old filename may survive only in this one-shot helper and negative guards.
stale = []
for path in root.rglob('*'):
    if not path.is_file() or '.git' in path.parts or path in {
        Path('tools/kernel-directory-tree-object-refactor.py'),
        Path('tests/native-kernel-maintainability-policy.sh'),
    }:
        continue
    try:
        text = path.read_text()
    except UnicodeDecodeError:
        continue
    if 'infiltratorfs_directory_tree.inc' in text:
        stale.append(str(path))
assert not stale, stale
