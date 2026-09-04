#!/usr/bin/env python3
from pathlib import Path

root = Path('.')
kernel = root / 'kernel'
data_path = kernel / 'infiltratorfs_rw_data.inc'
internal_path = kernel / 'infiltratorfs_internal.h'
make_path = kernel / 'Makefile'
policy_path = root / 'tests/native-kernel-maintainability-policy.sh'
cache_path = kernel / 'infiltratorfs_checksum_cache.c'

if cache_path.exists():
    raise SystemExit('checksum cache object already exists')
data = data_path.read_text()

# Remove cache-only sizing/state from the monolithic data unit.
for line in (
    '#define INFILFS_NATIVE_CHECKSUM_CACHE_SLOTS 256u\n',
    '#define INFILFS_NATIVE_CHECKSUM_GROUP_CACHE_SLOTS 4096u\n',
):
    if line not in data:
        raise SystemExit(f'missing expected cache macro: {line.strip()}')
    data = data.replace(line, '', 1)
state = '''static DEFINE_SPINLOCK(infilfs_native_checksum_cache_lock);\nstatic struct infilfs_native_checksum_cache_entry\n    infilfs_native_checksum_cache[INFILFS_NATIVE_CHECKSUM_CACHE_SLOTS];\nstatic struct infilfs_native_checksum_cache_entry\n    infilfs_native_checksum_group_cache[\n        INFILFS_NATIVE_CHECKSUM_GROUP_CACHE_SLOTS];\n'''
if state not in data:
    raise SystemExit('checksum cache state block not found')
data = data.replace(state, '', 1)

start = data.index('static void infilfs_native_checksum_cache_invalidate_sb(')
end = data.index('int infilfs_native_stage_block(', start)
body = data[start:end]
data = data[:start] + data[end:]
data_path.write_text(data)

# Keep hashing private to this object; the writer/index locator keeps its own
# same-TU helper in rw_data so no unrelated coupling is introduced.
hash_fn = '''static u32 infilfs_checksum_cache_id_hash(const u8 id[16])\n{\n    u32 h = 2166136261u;\n    unsigned int i;\n\n    for (i = 0; i < 16; ++i) {\n        h ^= id[i];\n        h *= 16777619u;\n    }\n    return h;\n}\n\n'''
body = body.replace('static void infilfs_native_checksum_cache_invalidate_sb(',
                    'void infilfs_native_checksum_cache_invalidate_sb(', 1)
body = body.replace('static bool infilfs_native_checksum_group_cache_lookup(',
                    'bool infilfs_native_checksum_group_cache_lookup(', 1)
body = body.replace('static void infilfs_native_checksum_group_cache_store(',
                    'void infilfs_native_checksum_group_cache_store(', 1)
body = body.replace('infilfs_native_id_hash(', 'infilfs_checksum_cache_id_hash(')
cache_path.write_text(
    '// SPDX-License-Identifier: GPL-3.0-or-later\n'
    '#include "infiltratorfs_internal.h"\n\n'
    '#define INFILFS_NATIVE_CHECKSUM_CACHE_SLOTS 256u\n'
    '#define INFILFS_NATIVE_CHECKSUM_GROUP_CACHE_SLOTS 4096u\n\n'
    'static DEFINE_SPINLOCK(infilfs_native_checksum_cache_lock);\n'
    'static struct infilfs_native_checksum_cache_entry\n'
    '    infilfs_native_checksum_cache[INFILFS_NATIVE_CHECKSUM_CACHE_SLOTS];\n'
    'static struct infilfs_native_checksum_cache_entry\n'
    '    infilfs_native_checksum_group_cache[\n'
    '        INFILFS_NATIVE_CHECKSUM_GROUP_CACHE_SLOTS];\n\n'
    + hash_fn + body)

internal = internal_path.read_text()
anchor = '''void infilfs_native_checksum_cache_store(\n    struct super_block *sb, const u8 owner_id[16], const u8 object_id[16],\n    u64 object_block, u64 start_logical);\n'''
extra = anchor + '''void infilfs_native_checksum_cache_invalidate_sb(struct super_block *sb);\nbool infilfs_native_checksum_group_cache_lookup(\n    struct super_block *sb, const u8 owner_id[16], u64 start_logical,\n    struct infilfs_native_checksum_cache_entry *out);\nvoid infilfs_native_checksum_group_cache_store(\n    struct super_block *sb, const u8 owner_id[16], const u8 object_id[16],\n    u64 object_block, u64 start_logical);\n'''
if anchor not in internal:
    raise SystemExit('checksum cache API anchor not found')
internal_path.write_text(internal.replace(anchor, extra, 1))

make = make_path.read_text()
old_line = 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o infiltratorfs_resize.o infiltratorfs_index_tree.o infiltratorfs_parallel_alloc.o infiltratorfs_allocation_publish.o infiltratorfs_read_cache.o infiltratorfs_pagecache.o infiltratorfs_directory_tree.o'
new_line = old_line + ' infiltratorfs_checksum_cache.o'
if old_line not in make:
    raise SystemExit('Kbuild object list not found')
make = make.replace(old_line, new_line, 1)
make = make.replace('#   infiltratorfs_read_cache.c owns verified read cursor/page caching.\n',
                    '#   infiltratorfs_read_cache.c owns verified read cursor/page caching.\n#   infiltratorfs_checksum_cache.c owns bounded checksum cursor/group caches.\n', 1)
make_path.write_text(make)

policy = policy_path.read_text()
old_assert = "grep -Fqx '" + old_line + "' \"$makefile\""
new_assert = "grep -Fqx '" + new_line + "' \"$makefile\""
if old_assert not in policy:
    raise SystemExit('maintainability Kbuild assertion not found')
policy = policy.replace(old_assert, new_assert, 1)
anchor_policy = "test -f \"$kernel/infiltratorfs_read_cache.c\" || fail 'verified-read cache object missing'\n"
extra_policy = anchor_policy + "test -f \"$kernel/infiltratorfs_checksum_cache.c\" || fail 'checksum cache object missing'\n"
if anchor_policy not in policy:
    raise SystemExit('maintainability cache anchor not found')
policy_path.write_text(policy.replace(anchor_policy, extra_policy, 1))

# The random-write policy should follow the cache sizing marker to its new owner.
rwp = root / 'tests/native-random-write-optimization-policy.sh'
rp = rwp.read_text()
rp = rp.replace('data="$root/kernel/infiltratorfs_rw_data.inc"\n',
                'data="$root/kernel/infiltratorfs_rw_data.inc"\ncache="$root/kernel/infiltratorfs_checksum_cache.c"\n', 1)
rp = rp.replace("grep -Fq 'INFILFS_NATIVE_CHECKSUM_GROUP_CACHE_SLOTS' \"$data\"",
                "grep -Fq 'INFILFS_NATIVE_CHECKSUM_GROUP_CACHE_SLOTS' \"$cache\"", 1)
rwp.write_text(rp)

# Ensure DKMS/package/native qualification source copies include the new object.
for p in [
    root / 'packaging/build-linux-packages.sh',
    root / '.github/workflows/kernel-module.yml',
    root / 'tests/native-complete-qualification.sh',
]:
    if not p.exists():
        continue
    text = p.read_text()
    if 'infiltratorfs_checksum_cache.c' in text:
        continue
    marker = 'infiltratorfs_rw_data.inc'
    if marker not in text:
        raise SystemExit(f'cannot add checksum cache to source manifest {p}')
    p.write_text(text.replace(marker, 'infiltratorfs_checksum_cache.c ' + marker))

readme = kernel / 'README.md'
doc = readme.read_text()
doc = doc.replace('`infiltratorfs_read_cache.c` owns verified-read caching,',
                  '`infiltratorfs_read_cache.c` owns verified-read caching, `infiltratorfs_checksum_cache.c` owns bounded checksum caches,', 1)
readme.write_text(doc)
