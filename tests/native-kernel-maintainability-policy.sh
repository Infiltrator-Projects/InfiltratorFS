#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
kernel="$root/kernel"
driver="$kernel/infiltratorfs_core.c"
rw="$kernel/infiltratorfs_rw.inc"
data="$kernel/infiltratorfs_rw_data.inc"
makefile="$kernel/Makefile"
ioctl="$kernel/infiltratorfs_ioctl.h"
resize="$kernel/infiltratorfs_resize.c"
quota="$kernel/infiltratorfs_quota.inc"
pagecache="$kernel/infiltratorfs_pagecache.c"

fail() {
    echo "native kernel maintainability policy: $*" >&2
    exit 1
}

for file in "$driver" "$rw" "$data" "$makefile" "$ioctl" "$resize" "$quota" "$pagecache"; do
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
grep -Fq 'max(1, online logical CPUs - 1)' "$ioctl" || \
    fail 'N-1 native CPU budget is missing from shipped synchronization contract'

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
grep -Fqx 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_crypto.o infiltratorfs_allocation_map.o infiltratorfs_resize.o infiltratorfs_index_tree.o infiltratorfs_extent_tree.o infiltratorfs_parallel_alloc.o infiltratorfs_allocation_publish.o infiltratorfs_read_cache.o infiltratorfs_pagecache.o infiltratorfs_directory_tree.o infiltratorfs_checksum_cache.o infiltratorfs_checksum_store.o infiltratorfs_locator_cache.o infiltratorfs_linux_meta_codec.o infiltratorfs_shared_ownership.o' "$makefile" || \
    fail 'kernel module is no longer built from explicit component objects'
test -f "$kernel/infiltratorfs_internal.h" || fail 'missing private kernel API header'
test -f "$kernel/infiltratorfs_crypto.c" || fail 'accelerated integrity object missing'
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
grep -Fq 'inode_get_mtime(inode)' "$data" || fail 'native writeback no longer persists VFS mtime'
grep -Fq 'inode_get_ctime(inode)' "$data" || fail 'native writeback no longer persists VFS ctime'
getattr_body="$(sed -n '/static int infilfs_getattr(/,/^}/p' "$rw")"
! grep -Fq 'write_lock' <<<"$getattr_body" || fail 'getattr regressed onto filesystem-wide topology lock'
! grep -Fq 'infilfs_read_object' <<<"$getattr_body" || fail 'getattr regressed to synchronous object reread'
grep -Fq 'stat->btime = ii->birth_time;' <<<"$getattr_body" || fail 'getattr lost cached persistent birth time'
grep -Fq 'struct timespec64 birth_time;' "$kernel/infiltratorfs_internal.h" || fail 'inode birth-time cache missing'
grep -Fq 'ATTR_KILL_SUID | ATTR_KILL_SGID' "$rw" || fail 'set-ID stripping is not persisted'

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

# Keep the page-cache bridge zero-copy at its folio/native-iterator boundary.
# Reintroducing MiB-scale read/write bounce buffers wastes memory bandwidth and
# prevents the adapter from scaling cleanly to multi-page folios.
grep -Fq '#include <linux/bvec.h>' "$pagecache" || \
    fail 'page-cache bvec contract include missing'
test "$(grep -Fc 'iov_iter_bvec(' "$pagecache")" -ge 4 || \
    fail 'page-cache read/write paths no longer use direct bvec iterators'
! grep -Fq 'cluster.buffer' "$pagecache" || \
    fail 'page-cache cluster bounce buffer returned'
! grep -Fq 'cluster->buffer' "$pagecache" || \
    fail 'page-cache submit path regressed to a bounce buffer'
! grep -Fq 'u8 *buffer;' "$pagecache" || \
    fail 'page-cache cluster buffer member returned'

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

# Writable mount latency must not scale with every regular-file object. Crash
# orphan discovery runs after mount, is fenced to the committed mount
# generation, and may only hold the topology read lock for bounded batches.
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
grep -Fq 'le64_to_cpu(header->generation) <= recovery_generation' <<<"$recovery_body" || \
    fail 'orphan recovery does not reject post-mount zero-link objects'
grep -Fq 'u32 end = min_t(u32, count, i + 256u);' <<<"$recovery_body" || \
    fail 'orphan discovery no longer yields the topology lock in bounded batches'
grep -Fq 'live_block = le64_to_cpu(entries[j].object_block);' <<<"$recovery_body" || \
    fail 'orphan discovery lost direct snapshot-block fast path'
grep -Fq 'infilfs_index_lookup(sb, entries[j].object_id' <<<"$recovery_body" || \
    fail 'orphan discovery lost moved-object fallback lookup'
test "$(grep -Fc 'down_read(&sbi->write_lock);' <<<"$recovery_body")" -ge 2 || \
    fail 'orphan discovery/revalidation lost topology serialization'
test "$(grep -Fc 'up_read(&sbi->write_lock);' <<<"$recovery_body")" -ge 3 || \
    fail 'orphan recovery no longer releases topology locks between phases'

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
# operation-table construction macros and caused false positives). The eighth
# legacy alias is mount_init: the public wrapper now adds SB_POSIXACL after the
# unchanged legacy mount-state initializer succeeds.
legacy_block="$(sed -n \
    '/^#define infilfs_rw_tx_begin infilfs_rw_tx_begin_legacy$/,/^#include "infiltratorfs_rw_legacy.inc"$/p' \
    "$rw")"
legacy_aliases="$(grep -Ec '^#define infilfs_[a-z0-9_]+[[:space:]]+infilfs_[a-z0-9_]+_legacy$' <<<"$legacy_block" || true)"
test "$legacy_aliases" -eq 8 || \
    fail "legacy alias bridge changed ($legacy_aliases entries; expected 8)"
grep -Fq '#define infilfs_rw_mount_init infilfs_rw_mount_init_legacy' <<<"$legacy_block" || \
    fail 'POSIX ACL mount-init alias bridge changed'

data_block="$(sed -n \
    '/^#define infilfs_rw_create __maybe_unused infilfs_rw_create_data$/,/^#include "infiltratorfs_rw_data.inc"$/p' \
    "$rw")"
data_aliases="$(grep -Ec '^#define infilfs_[a-z0-9_]+[[:space:]]+__maybe_unused[[:space:]]+infilfs_[a-z0-9_]+_data$' <<<"$data_block" || true)"
test "$data_aliases" -eq 3 || \
    fail "data alias bridge changed ($data_aliases entries; expected 3)"

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
check_bytes "$driver" 120000
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
check_bytes "$kernel/infiltratorfs_rw_namespace.inc" 100000
check_bytes "$quota" 70000

printf 'Native kernel locking/composition maintainability policy guard passed.\n'
