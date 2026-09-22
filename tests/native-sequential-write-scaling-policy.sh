#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
data="$root/kernel/infiltratorfs_rw_data.inc"
checksum_store="$root/kernel/infiltratorfs_checksum_store.c"
ns="$root/kernel/infiltratorfs_rw_namespace.inc"
internal="$root/kernel/infiltratorfs_internal.h"
pagecache="$root/kernel/infiltratorfs_pagecache.c"
read_cache="$root/kernel/infiltratorfs_read_cache.c"
core="$root/kernel/infiltratorfs_core.c"
linux_meta="$root/kernel/infiltratorfs_linux_meta.inc"
rw="$root/kernel/infiltratorfs_rw.inc"

# Sequential EOF appends must stay at the checksum tail rather than collecting
# the historical checksum chain on every group boundary.
grep -Fq 'infilfs_native_checksum_append_tail' "$checksum_store"
grep -Fq 'infilfs_native_writer_tail_lookup' "$checksum_store"
grep -Fq 'infilfs_native_writer_tail_store' "$checksum_store"
grep -Fq 'start_block == old_blocks' "$data"

append_body="$(sed -n '/static int infilfs_native_checksum_append_tail(/,/^}/p' "$checksum_store")"
! grep -Fq 'infilfs_native_checksum_collect' <<<"$append_body"

# Aligned sequential appends must prepare user bytes, integrity digests, the
# production IAC1 selection and unreachable reserved blocks before entering
# the filesystem-wide publication lock. Paged files must update only the tail
# leaf instead of flattening every historical extent page.
grep -Fq 'infilfs_native_prepare_append' "$data"
grep -Fq 'infs_iac1_compress_selected' "$data"
prepared_line="$(grep -n 'infilfs_native_prepare_append(' "$data" | tail -n1 | cut -d: -f1)"
lock_line="$(awk -v start="$prepared_line" '/down_write\(&sbi->write_lock\);/ && NR > start { print NR; exit }' "$data")"
test -n "$prepared_line" && test -n "$lock_line" && test "$prepared_line" -lt "$lock_line"

paged_append="$(sed -n '/static int infilfs_native_try_prepared_paged_append(/,/^}/p' "$data")"
grep -Fq 'old_pointers[old_page_count - 1u]' <<<"$paged_append"
grep -Fq 'infilfs_native_store_extent_page' <<<"$paged_append"
! grep -Fq 'infilfs_native_collect_extents' <<<"$paged_append"
grep -Fq 'prepared_paged_append_successes' "$internal"

# Paged index repoints/additions must resolve through the complete volatile
# locator rather than scanning every historical index page for every write.
grep -Fq 'infilfs_native_index_locator_build' "$data"
grep -Fq 'infilfs_native_index_locator_lookup' "$data"
grep -Fq 'infilfs_native_index_locator_insert' "$data"
index_body="$(sed -n '/static int infilfs_native_index_update_paged(/,/^}/p' "$data")"
grep -Fq 'located[c].page_index' <<<"$index_body"
grep -Fq 'infilfs_native_index_locator_lookup' <<<"$index_body"

# Metadata batching must not regress to one-second/1MiB synthetic pressure.
grep -Fq '#define INFILFS_NATIVE_IDLE_DELAY (5u * HZ)' "$data"
grep -Fq 'const u64 max_publish = 512ULL * 1024ULL * 1024ULL;' "$data"
grep -Fq '#define INFILFS_NATIVE_METADATA_PUBLISH_CHARGE (64ULL * 1024ULL)' "$ns"

# Publication drains only this transaction's CoW dependency set, makes those
# writes stable, then writes checkpoint replicas and performs the publication
# stable-media flush.
legacy="$root/kernel/infiltratorfs_rw_legacy.inc"
commit_body="$(sed -n '/static int infilfs_rw_tx_commit(/,/^}/p' "$legacy")"
! grep -Fq 'sync_blockdev(tx->sb->s_bdev)' <<<"$commit_body"
grep -Fq 'infilfs_rw_allocation_map_publish(tx, &next_allocation)' <<<"$commit_body"
grep -Fq 'infilfs_rw_sync_transaction_dependencies(tx)' <<<"$commit_body"
grep -Fq 'infilfs_rw_write_block_sync(tx->sb' <<<"$commit_body"
grep -Fq 'blkdev_issue_flush(tx->sb->s_bdev)' <<<"$commit_body"
test "$(grep -Fc 'blkdev_issue_flush(tx->sb->s_bdev)' <<<"$commit_body")" -eq 2
grep -Fq 'for (n = 1; n < INFILFS_CHECKPOINT_COUNT; ++n)' <<<"$commit_body"
dependency_sync="$(sed -n '/static int infilfs_rw_sync_transaction_dependencies(/,/^}/p' "$legacy")"
grep -Fq 'sb_find_get_block' <<<"$dependency_sync"
grep -Fq 'tx->allocated' <<<"$dependency_sync"
dependency_batch="$(sed -n '/static int infilfs_rw_sync_dependency_batch(/,/^}/p' "$legacy")"
grep -Fq 'blk_start_plug' <<<"$dependency_batch"
grep -Fq 'write_dirty_buffer' <<<"$dependency_batch"
grep -Fq 'wait_on_buffer' <<<"$dependency_batch"

# Deferred publication must react to excess physical CoW churn as well as
# logical user bytes so tiny partial writes cannot consume the volume before
# reaching the nominal logical threshold. The pending transaction state is a
# shared kernel object now, so the state field belongs in the private header;
# behaviour remains owned by the native data/namespace paths.
grep -Fq 'u64 pending_physical_bytes;' "$internal"
grep -Fq 'infilfs_native_pending_should_publish' "$data"
grep -Fq 'max_excess_churn = 64ULL * 1024ULL * 1024ULL' "$data"
grep -Fq 'infilfs_native_pending_should_publish(pending)' "$ns"


# Ordinary write(2) must enter the Linux page cache; the application thread
# must not synchronously run the native codec/CoW path. Read(2) shares that
# cache so freshly dirtied data remains coherent before writeback.
write_entry="$(sed -n '/static ssize_t infilfs_file_write_iter(/,/^}/p' "$data")"
grep -Fq 'generic_file_write_iter(iocb, from)' <<<"$write_entry"
grep -Fq 'iocb->ki_flags & IOCB_DIRECT' <<<"$write_entry"
grep -Fq 'infilfs_file_write_iter_common(iocb, from, true)' <<<"$write_entry"
grep -Fq '.open = infilfs_file_open' "$core"
grep -Fq 'FMODE_CAN_ODIRECT' "$core"
grep -Fq '.read_iter = infilfs_file_read_iter_dispatch' "$core"
direct_read="$(sed -n '/static ssize_t infilfs_file_read_iter_dispatch(/,/^}/p' "$rw")"
grep -Fq 'iocb->ki_flags & IOCB_DIRECT' <<<"$direct_read"
grep -Fq 'generic_file_read_iter(iocb, to)' <<<"$direct_read"
grep -Fq 'infilfs_native_writeback_batch_bytes()' "$pagecache"
grep -Fq 'infilfs_queue_cpu_work(&items[i].work)' "$data"
grep -Fq 'infilfs_writeback_cluster_submit' "$pagecache"
grep -Fq 'infilfs_native_writeback_iter' "$pagecache"
grep -Fq 'u64 persisted_size;' "$internal"
grep -Fq 'READ_ONCE(ii->persisted_size)' "$data"

# VM background writeback stages dirty folios into the same deferred
# transaction. It must not turn every writeback pass into a device-wide
# checkpoint publication; fsync/syncfs/unmount retain the durability boundary.
writepages="$(sed -n '/static int infilfs_writepages(/,/^}/p' "$pagecache")"
! grep -Fq 'infilfs_native_pending_flush_sb' <<<"$writepages"
fsync_body="$(sed -n '/static int infilfs_file_fsync(/,/^}/p' "$data")"
grep -Fq 'file_write_and_wait_range' <<<"$fsync_body"
grep -Fq 'infilfs_native_pending_flush_sb' <<<"$fsync_body"
pending_flush="$(sed -n '/int infilfs_native_pending_flush_sb(/,/^}/p' "$data")"
! grep -Fq 'sync_blockdev(sb->s_bdev)' <<<"$pending_flush"

# Verified multi-block reads submit a bounded 4 MiB run before waiting.
grep -Fq '#define INFILFS_NATIVE_READAHEAD_BLOCKS 1024u' "$read_cache"
grep -Fq '#define INFILFS_READ_IO_BATCH_BLOCKS 1024u' "$core"
read_run="$(sed -n '/static int infilfs_read_block_run(/,/^}/p' "$core")"
grep -Fq 'bh_read_batch' <<<"$read_run"
grep -Fq 'blk_start_plug' <<<"$read_run"
grep -Fq 'wait_on_buffer' <<<"$read_run"
readahead_body="$(sed -n '/static void infilfs_native_readahead_extent(/,/^}/p' "$read_cache")"
grep -Fq 'sb_breadahead' <<<"$readahead_body"

# Tree-directory i_blocks accounting must use the transaction's exact local
# allocation/free delta. Recursively rereading the growing directory tree after
# every create made both ordinary small-file creation and xattr sidecars
# quadratic in the number of entries.
grep -Fq 'infilfs_ns_rebuild_directory_accounted' "$ns"
grep -Fq 'infilfs_adjust_inode_blocks_after_commit' "$core"
create_body="$(sed -n '/static int infilfs_posix_create_native_named_child(/,/^}/p' "$rw")"
meta_delete_body="$(sed -n '/static int infilfs_linux_meta_delete_file(/,/^}/p' "$linux_meta")"
grep -Fq 'infilfs_ns_rebuild_directory_accounted' <<<"$create_body"
grep -Fq 'infilfs_adjust_inode_blocks_after_commit' <<<"$create_body"
! grep -Fq 'infilfs_refresh_inode_blocks_after_commit' <<<"$create_body"
grep -Fq 'infilfs_ns_rebuild_directory_accounted' <<<"$meta_delete_body"
! grep -Fq 'infilfs_refresh_inode_blocks_after_commit' <<<"$meta_delete_body"

printf 'Native sequential-write scaling policy guard passed.\n'
