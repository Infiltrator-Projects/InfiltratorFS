#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import re

root = Path('.')
kernel = root / 'kernel'
core = kernel / 'infiltratorfs_core.c'
internal = kernel / 'infiltratorfs_internal.h'
rw = kernel / 'infiltratorfs_rw.inc'
data_path = kernel / 'infiltratorfs_rw_data.inc'
legacy = kernel / 'infiltratorfs_rw_legacy.inc'
old_cache = kernel / 'infiltratorfs_rw_read_cache.inc'
new_cache = kernel / 'infiltratorfs_read_cache.c'
makefile = kernel / 'Makefile'
maint = Path('tests/native-kernel-maintainability-policy.sh')
readme = kernel / 'README.md'

assert old_cache.exists() and not new_cache.exists()

# Move the two small read-integrity state records and the checksum payload
# layout into the private module header. They were already shared implicitly
# by textual composition; this makes the boundary explicit without changing
# any on-disk structure or public ABI.
dt = data_path.read_text()

payload_pat = re.compile(
    r'struct infilfs_native_checksum_payload_disk \{.*?\} __packed;\n\n'
    r'#define INFILFS_NATIVE_CHECKSUMS_PER_OBJECT \\\n'
    r'(?:.*\\\n)*?.*\n', re.S)
m = payload_pat.search(dt)
assert m, 'checksum payload/macro block not found'
payload_block = m.group(0)
dt = dt[:m.start()] + dt[m.end():]

entry_pat = re.compile(r'struct infilfs_native_checksum_cache_entry \{.*?\};\n\n', re.S)
m = entry_pat.search(dt)
assert m, 'checksum cache entry not found'
entry_block = m.group(0)
dt = dt[:m.start()] + dt[m.end():]

cursor_pat = re.compile(
    r'#define INFILFS_NATIVE_READ_INTEGRITY_PARITY 1\n'
    r'struct infilfs_native_read_checksum_cursor \{.*?\};\n\n', re.S)
m = cursor_pat.search(dt)
assert m, 'read checksum cursor not found'
cursor_block = m.group(0)
dt = dt[:m.start()] + dt[m.end():]

# Read-cache services supplied by the data path gain ordinary module-private
# linkage. No behaviour or caller ordering changes.
for old, new in [
    ('static bool infilfs_native_checksum_cache_lookup(',
     'bool infilfs_native_checksum_cache_lookup('),
    ('static void infilfs_native_checksum_cache_store(',
     'void infilfs_native_checksum_cache_store('),
    ('static int infilfs_native_read_expected_digest(',
     'int infilfs_native_read_expected_digest('),
    ('static void infilfs_native_block_digest(',
     'void infilfs_native_block_digest('),
]:
    assert dt.count(old) == 1, (old, dt.count(old))
    dt = dt.replace(old, new, 1)
data_path.write_text(dt)

lt = legacy.read_text()
old = 'static int infilfs_rw_inline_digest('
assert lt.count(old) == 1
legacy.write_text(lt.replace(old, 'int infilfs_rw_inline_digest(', 1))

# Core extent/object helpers required by the independently compiled verified
# reader become private-module APIs.
ct = core.read_text()
for old, new in [
    ('static const u8 infilfs_extent_page_magic[8]',
     'const u8 infilfs_extent_page_magic[8]'),
    ('static u32 infilfs_extent_kind(', 'u32 infilfs_extent_kind('),
    ('static bool infilfs_extent_is_compressed(',
     'bool infilfs_extent_is_compressed('),
    ('static bool infilfs_extent_flags_valid(',
     'bool infilfs_extent_flags_valid('),
    ('static int infilfs_read_compressed_extent(',
     'int infilfs_read_compressed_extent('),
    ('static int infilfs_map_file_block_detail(',
     'int infilfs_map_file_block_detail('),
    ('static int infilfs_read_object(', 'int infilfs_read_object('),
]:
    assert ct.count(old) == 1, (old, ct.count(old))
    ct = ct.replace(old, new, 1)

# INFILFS_I() was another hidden same-translation-unit dependency. Move the
# trivial inode-private accessor into the module-private header so every
# compiled component uses one definition rather than open-coding the cast.
inode_accessor = '''static struct infilfs_inode_info *INFILFS_I(struct inode *inode)
{
    return inode->i_private;
}

'''
assert ct.count(inode_accessor) == 1
ct = ct.replace(inode_accessor, '', 1)
core.write_text(ct)

# Build the compiled read-cache source. Only the VFS entry point must escape
# this component; the cursor/page-search helpers remain file-private.
cache = old_cache.read_text()
cache = cache.replace('// SPDX-License-Identifier: GPL-3.0-or-later\n',
                      '// SPDX-License-Identifier: GPL-3.0-or-later\n#include "infiltratorfs_internal.h"\n', 1)
old = 'static ssize_t infilfs_file_read_iter_cached('
assert cache.count(old) == 1
cache = cache.replace(old, 'ssize_t infilfs_file_read_iter_cached(', 1)
new_cache.write_text(cache)
old_cache.unlink()

# Private header owns the cross-object read-integrity types and service API.
h = internal.read_text()
inode_anchor = '''struct infilfs_inode_info {
    u64 object_block;
    u64 data_allocation_hint;
    u64 portable_flags;
    u16 object_type;
    u8 object_id[16];
    char *symlink_target;
};
'''
assert inode_anchor in h
h = h.replace(inode_anchor, inode_anchor + '''
static inline struct infilfs_inode_info *INFILFS_I(struct inode *inode)
{
    return inode->i_private;
}
''', 1)
anchor = '''struct infilfs_visit_set {
    u64 *slots;
    size_t capacity;
    size_t count;
};
'''
assert anchor in h
h = h.replace(anchor, anchor + '\n' + payload_block + entry_block + cursor_block, 1)
api_anchor = '''int infilfs_rw_allocation_map_publish(
    struct infilfs_rw_tx *tx, struct infilfs_allocation_layout *next_layout);
'''
assert api_anchor in h
api = '''
/* Services shared with the compiled verified-read cursor/cache layer. */
extern const u8 infilfs_extent_page_magic[8];
u32 infilfs_extent_kind(u32 flags);
bool infilfs_extent_is_compressed(u32 flags);
bool infilfs_extent_flags_valid(u32 logical_blocks, u64 physical, u32 flags);
int infilfs_read_compressed_extent(
    struct inode *inode, u64 physical, u32 extent_blocks, u32 flags,
    u8 *plain, size_t plain_capacity);
int infilfs_map_file_block_detail(
    struct inode *inode, const u8 *object, u64 logical,
    u64 *physical_out, u32 *flags_out,
    u64 *extent_logical_out, u32 *extent_blocks_out);
int infilfs_read_object(
    struct super_block *sb, u64 object_block, u16 expected_type,
    const u8 *expected_id, u8 *out);
bool infilfs_native_checksum_cache_lookup(
    struct super_block *sb, const u8 owner_id[16],
    struct infilfs_native_checksum_cache_entry *out);
void infilfs_native_checksum_cache_store(
    struct super_block *sb, const u8 owner_id[16], const u8 object_id[16],
    u64 object_block, u64 start_logical);
int infilfs_native_read_expected_digest(
    struct super_block *sb, const u8 owner_id[16], const u8 head_id[16],
    u64 logical, struct infilfs_native_read_checksum_cursor *cursor,
    u8 checksum_object[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_data_checksum_disk *digest_out);
void infilfs_native_block_digest(
    const u8 data[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_data_checksum_disk *digest);
int infilfs_rw_inline_digest(const u8 *data, size_t size, u8 out[32]);
ssize_t infilfs_file_read_iter_cached(struct kiocb *iocb, struct iov_iter *to);
'''
h = h.replace(api_anchor, api_anchor + api, 1)
internal.write_text(h)

# Remove textual composition. Keep the existing callback alias so the VFS table
# remains behaviour-identical while the implementation is now separately linked.
rwt = rw.read_text()
inc = '#include "infiltratorfs_rw_read_cache.inc"\n'
assert inc in rwt
rwt = rwt.replace(inc, '', 1)
rw.write_text(rwt)

# Kbuild object seven.
mk = makefile.read_text()
old_line = ('infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o '
            'infiltratorfs_resize.o infiltratorfs_index_tree.o '
            'infiltratorfs_parallel_alloc.o infiltratorfs_allocation_publish.o')
new_line = old_line + ' infiltratorfs_read_cache.o'
assert old_line in mk
mk = mk.replace(old_line, new_line, 1)
marker = '#   infiltratorfs_allocation_publish.c owns CoW allocation-tree publication.\n'
assert marker in mk
mk = mk.replace(marker, marker +
    '#   infiltratorfs_read_cache.c owns verified read cursor/page caching.\n', 1)
makefile.write_text(mk)

# Package/DKMS/source-copy manifests now ship the compiled source.
for path in root.rglob('*'):
    if not path.is_file() or '.git' in path.parts or path == Path('tools/kernel-read-cache-object-refactor.py'):
        continue
    if path.suffix not in {'.sh', '.yml', '.yaml', '.md', '.txt', '.in', '.c', '.h'}:
        continue
    try:
        text = path.read_text()
    except UnicodeDecodeError:
        continue
    changed = text.replace('infiltratorfs_rw_read_cache.inc', 'infiltratorfs_read_cache.c')
    if changed != text:
        path.write_text(changed)

# Maintainability: exact seven-object layout, no textual read-cache include and
# remove it from the remaining compositor-order contract after filename migration.
mp = maint.read_text()
old_guard = "grep -Fqx 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o infiltratorfs_resize.o infiltratorfs_index_tree.o infiltratorfs_parallel_alloc.o infiltratorfs_allocation_publish.o' \"$makefile\" || \\\n"
new_guard = "grep -Fqx 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o infiltratorfs_resize.o infiltratorfs_index_tree.o infiltratorfs_parallel_alloc.o infiltratorfs_allocation_publish.o infiltratorfs_read_cache.o' \"$makefile\" || \\\n"
assert old_guard in mp
mp = mp.replace(old_guard, new_guard, 1)
anchor = "test -f \"$kernel/infiltratorfs_allocation_publish.c\" || fail 'allocation publisher object missing'\n"
assert anchor in mp
mp = mp.replace(anchor, anchor +
    "test -f \"$kernel/infiltratorfs_read_cache.c\" || fail 'verified-read cache object missing'\n"
    "test ! -e \"$kernel/infiltratorfs_rw_read_cache.inc\" || fail 'verified-read cache regressed to textual include'\n"
    "! grep -Fq 'infiltratorfs_rw_read_cache.inc' \"$rw\" || fail 'RW compositor textually includes verified-read cache'\n", 1)
mp = mp.replace('    infiltratorfs_read_cache.c\n', '', 1)
maint.write_text(mp)

# Local architecture prose follows the compiled boundary.
doc = readme.read_text()
old = ('`infiltratorfs_parallel_alloc.c` owns volatile sharded data reservations, and '
       '`infiltratorfs_allocation_publish.c` owns CoW allocation-tree publication.')
new = ('`infiltratorfs_parallel_alloc.c` owns volatile sharded data reservations, '
       '`infiltratorfs_allocation_publish.c` owns CoW allocation-tree publication, and '
       '`infiltratorfs_read_cache.c` owns verified read cursor/page caching.')
assert old in doc
doc = doc.replace(old, new, 1)
doc = doc.replace(
    'including read/write data paths, namespace mutation, directory trees, page cache, Linux metadata, quotas and defragmentation.',
    'including write-data paths, namespace mutation, directory trees, page cache, Linux metadata, quotas and defragmentation.',
    1)
doc = doc.replace(
    'prevents extracted allocation/index/resize/reservation/publication components from regressing into textual inclusion,',
    'prevents extracted allocation/index/resize/reservation/publication/read-cache components from regressing into textual inclusion,',
    1)
readme.write_text(doc)

# Old filename may survive only in this one-shot helper and negative guards.
stale = []
for path in root.rglob('*'):
    if not path.is_file() or '.git' in path.parts or path in {
        Path('tools/kernel-read-cache-object-refactor.py'),
        Path('tests/native-kernel-maintainability-policy.sh'),
    }:
        continue
    try:
        text = path.read_text()
    except UnicodeDecodeError:
        continue
    if 'infiltratorfs_rw_read_cache.inc' in text:
        stale.append(str(path))
assert not stale, stale
