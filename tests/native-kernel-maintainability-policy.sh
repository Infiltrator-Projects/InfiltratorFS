#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
kernel="$root/kernel"
driver="$kernel/infiltratorfs_core.c"
rw="$kernel/infiltratorfs_rw.inc"
data="$kernel/infiltratorfs_rw_data.inc"
namespace="$kernel/infiltratorfs_rw_namespace.inc"
makefile="$kernel/Makefile"
ioctl="$kernel/infiltratorfs_ioctl.h"
resize="$kernel/infiltratorfs_resize.c"
quota="$kernel/infiltratorfs_quota.inc"
pagecache="$kernel/infiltratorfs_pagecache.c"
orphan="$kernel/infiltratorfs_orphan_scan.c"

fail() {
    echo "native kernel maintainability policy: $*" >&2
    exit 1
}

for file in "$driver" "$rw" "$data" "$namespace" "$makefile" "$ioctl" "$resize" "$quota" "$pagecache" "$orphan"; do
    test -f "$file" || fail "missing $file"
done

# The synchronization contract must live with the shipped kernel source, not
# only in prose documentation. Keep Kbuild and the always-copied DKMS header in
# agreement about the permitted nesting directions.
for marker in \
    'resize_lock -> write_lock' \
    'quota_lock -> write_lock' \
    'write_lock -> bitmap_lock' \
    'allocation-reservation shard spinlock -> bitmap_lock' \
    'shared_range_build_lock -> write_lock (read side only)'; do
    grep -Fq "$marker" "$makefile" || fail "Makefile lost lock rule: $marker"
    grep -Fq "$marker" "$ioctl" || fail "ioctl header lost lock rule: $marker"
done
grep -Fq 'A path holding write_lock must never acquire resize_lock.' "$makefile" || \
    fail 'write_lock -> resize_lock prohibition is undocumented'
grep -Fq 'Ordinary data writers must not acquire quota_lock while holding' "$makefile" || \
    fail 'write_lock -> quota_lock prohibition is undocumented'
grep -Fq 'linux_meta_lock owns compound' "$ioctl" || \
    fail 'linux_meta_lock ownership is undocumented'
grep -Fq 'write_lock is the authoritative pending-transaction merge/checkpoint' "$ioctl" || \
    fail 'transaction/publication ownership is undocumented'
grep -Fq 'max(1, online physical cores - 1)' "$ioctl" || \
    fail 'physical-core N-1 native CPU budget is missing from shipped synchronization contract'

# Keep the geometry path aligned with the declared resize_lock -> write_lock
# order. The second lock must be acquired after resize_lock in the public resize
# wrapper, never the reverse.
resize_outer="$(grep -nF 'mutex_lock(&sbi->resize_lock);' "$resize" | tail -n1 | cut -d: -f1)"
resize_inner="$(awk -v start="${resize_outer:-0}" '/down_write\(&sbi->write_lock\);/ && NR > start { print NR; exit }' "$resize")"
test -n "$resize_outer" || fail 'resize_lock acquisition not found'
test -n "$resize_inner" || fail 'write_lock acquisition after resize_lock not found'
(( resize_outer < resize_inner )) || fail 'resize lock order reversed'

# Quota administration has paths that intentionally enter persistent mutation
# while quota_lock is held. Verify both domains remain present; deeper call-flow
# behaviour is covered by the mounted quota qualification rather than a fragile
# text parser.
grep -Fq 'mutex_lock(&sbi->quota_lock);' "$quota" || fail 'quota_lock acquisition missing'
grep -Fq 'down_read(&sbi->write_lock);' "$quota" || fail 'quota topology read lock acquisition missing'

# The native driver must stay a genuine multi-object Kbuild module. The
# allocation map is the first extracted subsystem and must never regress into
# textual inclusion.
grep -Fqx 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_cpu.o infiltratorfs_crypto.o infiltratorfs_security.o infiltratorfs_extension.o infiltratorfs_allocation_map.o infiltratorfs_resize.o infiltratorfs_index_tree.o infiltratorfs_extent_tree.o infiltratorfs_parallel_alloc.o infiltratorfs_allocation_publish.o infiltratorfs_read_cache.o infiltratorfs_pagecache.o infiltratorfs_directory_tree.o infiltratorfs_checksum_cache.o infiltratorfs_checksum_store.o infiltratorfs_locator_cache.o infiltratorfs_linux_meta_codec.o infiltratorfs_shared_ownership.o infiltratorfs_orphan_scan.o infiltratorfs_name_policy.o' "$makefile" || \
    fail 'kernel module is no longer built from explicit component objects'
test -f "$kernel/infiltratorfs_internal.h" || fail 'missing private kernel API header'
test -f "$kernel/infiltratorfs_cpu.c" || fail 'native CPU policy object missing'
test -f "$kernel/infiltratorfs_crypto.c" || fail 'accelerated integrity object missing'
test -f "$kernel/infiltratorfs_security.c" || fail 'portable security validator object missing'
test -f "$kernel/infiltratorfs_extension.c" || fail 'typed extension validator object missing'
test -f "$kernel/infiltratorfs_name_policy.c" || fail 'native filename-policy object missing'
test -f "$kernel/infiltratorfs_allocation_map.c" || fail 'allocation map object missing'
test -f "$kernel/infiltratorfs_index_tree.c" || fail 'object-index tree object missing'
test -f "$kernel/infiltratorfs_extent_tree.c" || fail 'extent-tree object missing'
test -f "$kernel/infiltratorfs_parallel_alloc.c" || fail 'parallel allocator object missing'
test -f "$kernel/infiltratorfs_allocation_publish.c" || fail 'allocation publisher object missing'
test -f "$kernel/infiltratorfs_read_cache.c" || fail 'verified-read cache object missing'
test -f "$kernel/infiltratorfs_checksum_cache.c" || fail 'checksum cache object missing'
test -f "$kernel/infiltratorfs_checksum_store.c" || fail 'checksum store object missing'
test -f "$kernel/infiltratorfs_pagecache.c" || fail 'page-cache object missing'
test -f "$kernel/infiltratorfs_directory_tree.c" || fail 'directory-tree object missing'
test -f "$kernel/infiltratorfs_linux_meta_codec.c" || fail 'Linux metadata codec object missing'
test -f "$kernel/infiltratorfs_locator_cache.c" || fail 'native locator cache object missing'
test -f "$kernel/infiltratorfs_shared_ownership.c" || fail 'shared ownership accelerator object missing'
test -f "$kernel/infiltratorfs_orphan_scan.c" || fail 'parallel orphan scanner object missing'
grep -Fq 'kernel/infiltratorfs_orphan_scan.c \' "$root/.github/workflows/kernel-module.yml" || \
    fail 'DKMS qualification source root omits orphan scanner component'
grep -Fqx '#   infiltratorfs_orphan_scan.c owns N-1 parallel crash-orphan discovery.' "$makefile" || \
    fail 'orphan scanner ownership line is not a valid Makefile comment'
visit_line="$(grep -nF 'struct infilfs_visit_set {' "$kernel/infiltratorfs_internal.h" | head -n1 | cut -d: -f1)"
pending_line="$(grep -nF 'struct infilfs_native_pending {' "$kernel/infiltratorfs_internal.h" | head -n1 | cut -d: -f1)"
test -n "$visit_line" && test -n "$pending_line" && (( visit_line < pending_line )) || \
    fail 'visit-set definition must precede native-pending embedded use'

# Every compiled Kbuild object must also ship in the self-contained DKMS source
# installed by the Debian/.run packaging path. A repository build can succeed
# while package installation fails later if a newly split object is omitted.
package_builder="$root/packaging/build-linux-packages.sh"
test -f "$package_builder" || fail 'Linux package builder missing'
module_objects="$(sed -nE 's/^infiltratorfs-y := (.*)$/\1/p' "$makefile")"
test -n "$module_objects" || fail 'could not resolve Kbuild object list'
for object in $module_objects; do
    source="${object%.o}.c"
    grep -Fq "$source" "$package_builder" || \
        fail "DKMS package source list omits $source"
done

test ! -e "$kernel/infiltratorfs_directory_tree.inc" || fail 'directory tree regressed to textual include'
! grep -Fq 'infiltratorfs_directory_tree.inc' "$rw" || fail 'RW compositor textually includes directory tree'
test ! -e "$kernel/infiltratorfs_pagecache.inc" || fail 'page-cache regressed to textual include'
! grep -Fq 'infiltratorfs_pagecache.inc' "$rw" || fail 'RW compositor textually includes page cache'
test ! -e "$kernel/infiltratorfs_rw_read_cache.inc" || fail 'verified-read cache regressed to textual include'
! grep -Fq 'infiltratorfs_rw_read_cache.inc' "$rw" || fail 'RW compositor textually includes verified-read cache'
test ! -e "$kernel/infiltratorfs_allocation_publish.inc" || fail 'allocation publisher regressed to textual include'
! grep -Fq 'infiltratorfs_allocation_publish.inc' "$rw" || fail 'RW compositor textually includes allocation publisher'
test ! -e "$kernel/infiltratorfs_parallel_alloc.inc" || fail 'parallel allocator regressed to textual include'
! grep -Fq 'infiltratorfs_parallel_alloc.inc' "$rw" || fail 'RW compositor textually includes parallel allocator'
test ! -e "$kernel/infiltratorfs_index_tree.inc" || fail 'object-index tree regressed to textual include'
! grep -Fq 'infiltratorfs_index_tree.inc' "$driver" || fail 'core textually includes object-index tree'
test -f "$kernel/infiltratorfs_resize.c" || fail 'resize object missing'
test ! -e "$kernel/infiltratorfs_resize.inc" || fail 'resize regressed to textual include'
! grep -Fq 'infiltratorfs_resize.inc' "$driver" || fail 'core textually includes resize'
test ! -e "$kernel/infiltratorfs_allocation_map.inc" || fail 'allocation map regressed to textual include'
! grep -Fq 'infiltratorfs_allocation_map.inc' "$driver" || fail 'core textually includes allocation map'

# Linux VFS identity and write semantics must remain explicit in the native
# adapter. The on-disk 128-bit object ID, not the compact i_ino hash, is the
# inode-cache identity; append selection, privilege removal and sync flags use
# the standard VFS helpers.
grep -Fq 'iget5_locked' "$driver" || fail 'inode cache no longer matches full object identity'
grep -Fq 'memcmp(ii->object_id, args->object_id, 16)' "$driver" || \
    fail 'inode cache does not compare the full 128-bit object ID'
write_common="$(sed -n '/static ssize_t infilfs_file_write_iter_common(/,/^}/p' "$data")"
grep -Fq 'inode_lock(inode)' <<<"$write_common" || fail 'same-inode writes are not serialized'
grep -Fq 'generic_write_checks(iocb, from)' <<<"$write_common" || fail 'generic write contract is bypassed'
grep -Fq 'file_remove_privs(filep)' <<<"$write_common" || fail 'writes do not strip file privileges'
grep -Fq 'file_update_time(filep)' <<<"$write_common" || fail 'direct native writes do not update VFS timestamps'
grep -Fq 'generic_write_sync(iocb, ret)' <<<"$write_common" || fail 'sync write flags are ignored'
write_buffered="$(sed -n '/static ssize_t infilfs_file_write_iter(struct kiocb /,/^}/p' "$data")"
grep -Fq 'generic_write_checks(iocb, from)' <<<"$write_buffered" || fail 'buffered writes bypass generic write checks'
grep -Fq 'file_remove_privs(filep)' <<<"$write_buffered" || fail 'buffered writes do not strip file privileges'
grep -Fq 'file_update_time(filep)' <<<"$write_buffered" || fail 'buffered writes do not update VFS timestamps'
grep -Fq '__generic_file_write_iter(iocb, from)' <<<"$write_buffered" || fail 'buffered write path lost generic page-cache write'
grep -Fq 'iocb->ki_flags & IOCB_DIRECT' <<<"$write_buffered" || fail 'direct writes are not dispatched explicitly'
grep -Fq 'infilfs_file_write_iter_common(iocb, from, true)' <<<"$write_buffered" || fail 'direct writes lost native verified path'
grep -Fq 'FMODE_CAN_ODIRECT' "$driver" || fail 'O_DIRECT admission is not advertised to modern VFS'
grep -Fq '.read_iter = infilfs_file_read_iter_dispatch' "$driver" || fail 'direct-read dispatcher is not active'
direct_read="$(sed -n '/static ssize_t infilfs_file_read_iter_dispatch(/,/^}/p' "$rw")"
grep -Fq 'iocb->ki_flags & IOCB_DIRECT' <<<"$direct_read" || fail 'direct reads are not dispatched explicitly'
grep -Fq 'generic_file_read_iter(iocb, to)' <<<"$direct_read" || fail 'ordinary reads no longer use page cache'
grep -Fq 'inode_get_mtime(inode)' "$data" || fail 'native writeback no longer persists VFS mtime'
grep -Fq 'inode_get_ctime(inode)' "$data" || fail 'native writeback no longer persists VFS ctime'
rewrite_body="$(sed -n '/static int infilfs_posix_rewrite_inode(/,/^}/p' "$rw")"
grep -Fq 'infilfs_ns_finish_locked(pending, ret)' <<<"$rewrite_body" || \
    fail 'POSIX metadata rewrite releases topology lock before VFS timestamp publication'
grep -Fq 'inode_set_mtime_to_ts(inode, attr->ia_mtime)' <<<"$rewrite_body" || \
    fail 'POSIX metadata rewrite does not publish mtime under topology lock'
grep -Fq 'inode_set_ctime_to_ts(inode, attr->ia_ctime)' <<<"$rewrite_body" || \
    fail 'POSIX metadata rewrite does not publish ctime under topology lock'
setattr_body="$(sed -n '/static int infilfs_posix_setattr(/,/^}/p' "$rw")"
test "$(grep -Fc 'filemap_write_and_wait(inode->i_mapping)' <<<"$setattr_body")" -eq 1 || \
    fail 'setattr reintroduced a per-mtime buffered-write drain'
getattr_body="$(sed -n '/static int infilfs_getattr(/,/^}/p' "$rw")"
! grep -Fq 'write_lock' <<<"$getattr_body" || fail 'getattr regressed onto filesystem-wide topology lock'
! grep -Fq 'infilfs_read_object' <<<"$getattr_body" || fail 'getattr regressed to synchronous object reread'
grep -Fq 'stat->btime = ii->birth_time;' <<<"$getattr_body" || fail 'getattr lost cached persistent birth time'
grep -Fq 'struct timespec64 birth_time;' "$kernel/infiltratorfs_internal.h" || fail 'inode birth-time cache missing'
grep -Fq 'ATTR_KILL_SUID | ATTR_KILL_SGID' "$rw" || fail 'set-ID stripping is not persisted'

# The portable core can encode per-object replication/encryption policy that
# the single-bdev native Linux adapter cannot yet service. The kernel must fail
# closed at inode admission instead of treating policy-encoded data as ordinary
# raw extents and risking silent corruption.
grep -Fq 'ii->portable_flags & INFILFS_ATTR_STORAGE_POLICY_MASK' "$driver" || \
    fail 'native Linux does not reject unsupported per-object storage policy'
grep -Fq 'native Linux cannot open file with nondefault storage policy' "$driver" || \
    fail 'native Linux storage-policy rejection is not diagnosable'

# Creator identity conversion is fail-closed. An id-mapped uid/gid that cannot
# be represented in the init-user-namespace disk fields must never fall back to
# numeric zero and manufacture root ownership.
creator_fill="$(sed -n '/static int infilfs_posix_fill_common_attributes(/,/^}/p' "$rw")"
grep -Fq 'ret = infilfs_posix_uid_to_disk(kernel_uid, &uid);' <<<"$creator_fill" || \
    fail 'creator uid conversion result is not checked'
grep -Fq 'ret = infilfs_posix_gid_to_disk(kernel_gid, &gid);' <<<"$creator_fill" || \
    fail 'creator gid conversion result is not checked'
test "$(grep -Fc 'if (ret)' <<<"$creator_fill")" -ge 2 || \
    fail 'creator identity conversion no longer fails closed'
create_object="$(sed -n '/static int infilfs_posix_create_object_native(/,/^}/p' "$rw")"
test "$(grep -Fc 'ret = infilfs_posix_fill_common_attributes(' <<<"$create_object")" -eq 2 || \
    fail 'native file/directory creation bypasses checked creator identity'
symlink_object="$(sed -n '/static int infilfs_ns_create_symlink_object(/,/^}/p' "$namespace")"
grep -Fq 'ret = infilfs_rw_fill_common_attributes(' <<<"$symlink_object" || \
    fail 'native symlink creation bypasses checked creator identity'
grep -Fq 'if (ret)' <<<"$symlink_object" || \
    fail 'native symlink creator identity failure is ignored'

# VM writeback accounting is a page count, not a batch count. Keep the
# decrement independent of the cluster-full short circuit so a full 1 MiB
# batch cannot accidentally escape nr_to_write accounting. The direct
# large-folio fallback and the clustered path both account base pages.
! grep -Fq -- '--wbc->nr_to_write' "$pagecache" || \
    fail 'writeback accounting regressed to short-circuit pre-decrement'
! grep -Fq 'size_t *lengths;' "$pagecache" || \
    fail 'unused writeback cluster lengths array returned'
test "$(grep -Fc '(long)(folio_size(folio) >> PAGE_SHIFT)' "$pagecache")" -ge 2 || \
    fail 'writeback paths do not account every base page in a folio'
grep -Fq 'folio_test_private_2(folio)' "$pagecache" || \
    fail 'pending CoW accounting no longer uses the auxiliary folio flag'
grep -Fq 'folio_set_private_2(folio)' "$pagecache" || \
    fail 'pending CoW accounting marker is not set independently'
grep -Fq 'folio_clear_private_2(folio)' "$pagecache" || \
    fail 'pending CoW accounting marker is not cleared independently'
! grep -Fq 'folio_attach_private(folio, sbi)' "$pagecache" || \
    fail 'InfiltratorFS reclaimed folio->private from page-cache frameworks'

# Filesystems implementing ->writepages must provide folio migration or Linux
# falls back to a WARN_ON path during compaction. The adapter must also transfer
# InfiltratorFS's independent PG_private_2 pending-CoW marker without changing
# its superblock-wide accounting value.
grep -Fq '.migrate_folio = infilfs_migrate_folio' "$pagecache" || \
    fail 'page-cache folio migration callback missing'
migrate_body="$(sed -n '/static int infilfs_migrate_folio(/,/^}/p' "$pagecache")"
grep -Fq 'filemap_migrate_folio(mapping, dst, src, mode)' <<<"$migrate_body" || \
    fail 'folio migration does not delegate normal page-cache state'
grep -Fq 'folio_test_private_2(src)' <<<"$migrate_body" || \
    fail 'folio migration lost pending-CoW marker detection'
grep -Fq 'folio_set_private_2(dst)' <<<"$migrate_body" || \
    fail 'folio migration does not transfer pending-CoW marker'
grep -Fq 'folio_clear_private_2(src)' <<<"$migrate_body" || \
    fail 'folio migration leaves duplicate pending-CoW marker state'

# Reads retain direct bvec transport. Writeback deliberately snapshots each
# bounded dirty cluster before releasing folio locks: native compression/CoW
# and checkpoint publication may sleep for seconds under adverse media states,
# and must never pin ordinary page-cache locks for that entire interval.
grep -Fq '#include <linux/bvec.h>' "$pagecache" || \
    fail 'page-cache bvec contract include missing'
test "$(grep -Fc 'iov_iter_bvec(' "$pagecache")" -ge 4 || \
    fail 'page-cache read/write paths no longer use direct bvec iterators'
writeback_submit="$(sed -n '/static int infilfs_writeback_cluster_submit(/,/^}/p' "$pagecache")"
grep -Fq 'staged = kvmalloc(cluster->bytes, GFP_NOFS);' <<<"$writeback_submit" || \
    fail 'writeback no longer snapshots data before releasing folio locks'
grep -Fq 'folio_unlock(cluster->folios[i]);' <<<"$writeback_submit" || \
    fail 'writeback cluster keeps folio locks across native publication'
grep -Fq 'iov_iter_kvec(' <<<"$writeback_submit" || \
    fail 'staged writeback image is not handed to the native writer'
grep -Fq 'folio_end_writeback(folio);' <<<"$writeback_submit" || \
    fail 'staged writeback lost VFS completion accounting'
! grep -Fq 'cluster.buffer' "$pagecache" || \
    fail 'persistent page-cache cluster buffer member returned'
! grep -Fq 'cluster->buffer' "$pagecache" || \
    fail 'persistent page-cache submit buffer member returned'

# Linux 7.0 gained an iomap read transport hook that lets filesystems retain
# custom verified/compressed I/O while delegating folio state management to
# iomap. Keep generic bio iomap reads out of InfiltratorFS: they would bypass
# the native SHA-256 and compression reader.
grep -Fq 'KERNEL_VERSION(7, 0, 0) && IS_ENABLED(CONFIG_FS_IOMAP)' "$pagecache" || \
    fail 'verified iomap read compatibility boundary missing'
grep -Fq 'struct iomap_read_ops infilfs_iomap_verified_read_ops' "$pagecache" || \
    fail 'verified iomap read operations missing'
grep -Fq 'iomap_read_folio(&infilfs_iomap_read_ops' "$pagecache" || \
    fail 'Linux 7.0 read_folio does not delegate folio state to iomap'
grep -Fq 'iomap_readahead(&infilfs_iomap_read_ops' "$pagecache" || \
    fail 'Linux 7.0 readahead does not delegate folio state to iomap'
grep -Fq 'iomap_finish_folio_read(' "$pagecache" || \
    fail 'custom iomap transport does not complete folio ranges'
grep -Fq 'iomap->addr = IOMAP_NULL_ADDR;' "$pagecache" || \
    fail 'iomap read mapping no longer advertises custom transport'
! grep -Fq 'iomap_bio_read_ops' "$pagecache" || \
    fail 'generic iomap bio read would bypass InfiltratorFS verification'
! grep -Fq 'iomap_bio_read_folio' "$pagecache" || \
    fail 'generic iomap folio bio read would bypass InfiltratorFS verification'

# Large page-cache folios are enabled only once the VFS write contract is
# folio-native.  Bound the maximum order to one compression cluster so random
# dirtying cannot turn into unbounded CoW amplification.
grep -Fq '#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)' "$driver" || \
    fail 'large-folio kernel-version boundary missing'
grep -Fq 'mapping_set_folio_order_range(' "$driver" || \
    fail 'folio-native kernels no longer enable bounded large folios'
grep -Fq 'get_order((unsigned long)INFILFS_COMPRESSION_CLUSTER_BLOCKS *' "$driver" || \
    fail 'large-folio maximum is no longer tied to the compression cluster'

# Checkpoint selection already authenticates and expands the live allocation
# tree. Preserve that selected runtime state across the mount boundary rather
# than rereading the entire allocation tree in RW initialization.
checkpoint_graph_body="$(sed -n '/static int infilfs_validate_checkpoint_graph(/,/^}/p' "$driver")"
grep -Fq 'struct infilfs_allocation_layout committed_layout = {0};' <<<"$checkpoint_graph_body" || \
    fail 'checkpoint validation no longer retains allocation-tree geometry'
grep -Fq 'sb, candidate, &bitmap, &bitmap_bytes, &committed_layout);' <<<"$checkpoint_graph_body" || \
    fail 'checkpoint validation stopped loading transferable allocation state'
grep -Fq 'sbi->bitmap = bitmap;' <<<"$checkpoint_graph_body" || \
    fail 'validated allocation bitmap is not transferred to mount state'
grep -Fq 'infilfs_allocation_cache_replace(sbi, &committed_layout);' <<<"$checkpoint_graph_body" || \
    fail 'validated allocation-tree geometry is not transferred to mount state'
mount_init_body="$(sed -n '/int infilfs_rw_mount_init(struct super_block \*sb)/,/^}/p' "$kernel/infiltratorfs_rw_legacy.inc")"
grep -Fq 'if (sbi->bitmap)' <<<"$mount_init_body" || \
    fail 'RW mount init no longer adopts checkpoint-selected allocation state'
grep -Fq 'infilfs_allocation_cache_view(' <<<"$mount_init_body" || \
    fail 'RW mount init does not validate transferred allocation geometry'
grep -Fq '} else {' <<<"$mount_init_body" || \
    fail 'RW mount init lost defensive allocation-map fallback'

# Writable mount latency must not scale with every regular-file object. Crash
# orphan discovery runs after mount, is fenced to the committed mount
# generation, and scans disjoint catalogue ranges through the native N-1 pool.
# Live namespace mutation must never wait for the complete background scan.
grep -Fq 'infilfs_schedule_orphan_recovery(sb);' "$driver" || \
    fail 'writable mount lost deferred orphan recovery'
fill_super_body="$(sed -n '/static int infilfs_fill_super(/,/^}/p' "$driver")"
! grep -Fq 'ret = infilfs_native_recover_unlinked_files(sb);' <<<"$fill_super_body" || \
    fail 'full orphan scan regressed onto the synchronous mount path'
! grep -Fq 'infilfs_wait_for_orphan_recovery' "$kernel/infiltratorfs_rw_namespace.inc" || \
    fail 'namespace mutation waits for complete orphan recovery'
grep -Fq 'orphan_recovery_generation' "$kernel/infiltratorfs_internal.h" || \
    fail 'orphan recovery lost its mount-generation fence'
grep -Fq 'infilfs_mod_delayed_cpu_work(&sbi->orphan_recovery_work, 1);' "$driver" || \
    fail 'orphan recovery is not scheduled on the native unbound CPU pool'
recovery_body="$(sed -n '/static int infilfs_native_recover_unlinked_files(/,/^}/p' "$rw")"
grep -Fq 'infilfs_orphan_discover_parallel(' <<<"$recovery_body" || \
    fail 'orphan recovery lost parallel discovery'
grep -Fq 'le64_to_cpu(header->generation) <= recovery_generation' <<<"$recovery_body" || \
    fail 'orphan reclaim does not reject post-mount zero-link objects'
scan_body="$(sed -n '/static int infilfs_orphan_scan_range(/,/^}/p' "$orphan")"
grep -Fq 'u32 end = min_t(u32, item->end, i + 256u);' <<<"$scan_body" || \
    fail 'orphan discovery no longer yields the topology lock in bounded batches'
grep -Fq 'live_block = le64_to_cpu(entry->object_block);' <<<"$scan_body" || \
    fail 'orphan discovery lost direct snapshot-block fast path'
grep -Fq 'infilfs_index_lookup(' <<<"$scan_body" || \
    fail 'orphan discovery lost moved-object fallback lookup'
grep -Fq 'down_read(&sbi->write_lock);' <<<"$scan_body" || \
    fail 'parallel orphan discovery lost topology serialization'
grep -Fq 'up_read(&sbi->write_lock);' <<<"$scan_body" || \
    fail 'parallel orphan discovery no longer releases topology locks between batches'
grep -Fq 'infilfs_queue_cpu_work(&work[i].work)' "$orphan" || \
    fail 'orphan discovery no longer dispatches independent N-1 scan ranges'

# Only the core object and the explicit RW compositor may textually compose
# remaining implementation .inc units. A leaf .inc importing another leaf creates hidden
# ordering/cycle dependencies and is rejected.
while IFS= read -r file; do
    case "$file" in
        "$driver"|"$rw") ;;
        *) fail "unexpected nested kernel implementation include: $file" ;;
    esac
done < <(grep -RIlE '#include[[:space:]]+"infiltratorfs_[^"]+\.inc"' \
    "$kernel" --include='*.c' --include='*.inc')

# Preserve the known RW layer order. This is deliberately an ordering contract,
# not a claim that the current macro-composition mechanism should live forever.
ordered=(
    infiltratorfs_rw_legacy.inc
    infiltratorfs_rw_data.inc
    infiltratorfs_rw_namespace.inc
    infiltratorfs_linux_meta.inc
)
previous=0
for include in "${ordered[@]}"; do
    line="$(grep -nF "#include \"$include\"" "$rw" | head -n1 | cut -d: -f1)"
    test -n "$line" || fail "RW compositor lost $include"
    (( line > previous )) || fail "RW compositor order changed at $include"
    previous="$line"
done

# Macro-renamed entry points are migration debt. Guard the known alias bridges
# structurally instead of counting every macro in rw.inc (which also contains
# operation-table construction macros and caused false positives). The seventh
# retained legacy alias is mount_init: the public wrapper now adds SB_POSIXACL
# after the unchanged legacy mount-state initializer succeeds. The obsolete
# legacy fsync alias is intentionally gone; the active data-layer fsync is the
# only implementation allowed to reach the VFS.
legacy_block="$(sed -n \
    '/^#define infilfs_rw_tx_begin infilfs_rw_tx_begin_legacy$/,/^#include "infiltratorfs_rw_legacy.inc"$/p' \
    "$rw")"
legacy_aliases="$(grep -Ec '^#define infilfs_[a-z0-9_]+[[:space:]]+infilfs_[a-z0-9_]+_legacy$' <<<"$legacy_block" || true)"
test "$legacy_aliases" -eq 7 || \
    fail "legacy alias bridge changed ($legacy_aliases entries; expected 7)"
grep -Fq '#define infilfs_rw_mount_init infilfs_rw_mount_init_legacy' <<<"$legacy_block" || \
    fail 'POSIX ACL mount-init alias bridge changed'

grep -Fq '#include "infiltratorfs_rw_data.inc"' "$rw" || \
    fail 'RW data compositor include missing'
! grep -Fq '#define infilfs_rw_create __maybe_unused infilfs_rw_create_data' "$rw" || \
    fail 'retired create data alias bridge returned'
! grep -Fq '#define infilfs_rw_mkdir __maybe_unused infilfs_rw_mkdir_data' "$rw" || \
    fail 'retired mkdir data alias bridge returned'
! grep -Fq '#define infilfs_rw_setattr __maybe_unused infilfs_rw_setattr_data' "$rw" || \
    fail 'retired setattr data alias bridge returned'
! grep -Fq 'infilfs_rw_create_legacy' "$data" || \
    fail 'RW data layer regained legacy create bridge'
! grep -Fq 'infilfs_rw_mkdir_legacy' "$data" || \
    fail 'RW data layer regained legacy mkdir bridge'
! grep -Fq 'infilfs_rw_setattr_legacy' "$data" || \
    fail 'RW data layer regained legacy setattr bridge'
grep -Fq '#define infilfs_file_read_iter infilfs_file_read_iter_atime' "$rw" || \
    fail 'read-cache/atime alias bridge changed'
grep -Fq '#define infilfs_rw_fill_common_attributes infilfs_posix_fill_common_attributes' "$rw" || \
    fail 'POSIX attribute alias bridge changed'

# Do not let the single-TU implementation silently become larger while it is
# being retired layer-by-layer. These ceilings leave practical edit headroom
# over the current sources but force deliberate decomposition before another
# major subsystem is added to the same textual composition.
check_bytes() {
    local file="$1" limit="$2" bytes
    bytes="$(wc -c < "$file")"
    (( bytes <= limit )) || fail "$file is $bytes bytes (ceiling $limit)"
}
check_bytes "$driver" 118000
check_bytes "$rw" 90000
check_bytes "$kernel/infiltratorfs_rw_legacy.inc" 100000
check_bytes "$kernel/infiltratorfs_rw_data.inc" 170000
! grep -Fq 'static int infilfs_native_checksum_decode(' "$kernel/infiltratorfs_rw_data.inc" || \
    fail 'persistent checksum store regressed into RW data compositor'
grep -Fq 'int infilfs_native_checksum_decode(' "$kernel/infiltratorfs_checksum_store.c" || \
    fail 'compiled checksum store lost checksum-object ownership'
! grep -Fq 'static int infilfs_native_index_locator_build(' "$kernel/infiltratorfs_rw_data.inc" || \
    fail 'locator cache regressed into RW data compositor'
grep -Fq 'int infilfs_native_index_locator_build(' "$kernel/infiltratorfs_locator_cache.c" || \
    fail 'compiled locator cache lost object-index locator ownership'
check_bytes "$kernel/infiltratorfs_rw_namespace.inc" 90000
check_bytes "$quota" 70000

printf 'Native kernel locking/composition maintainability policy guard passed.\n'
