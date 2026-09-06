#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
data="$root/kernel/infiltratorfs_rw_data.inc"
ns="$root/kernel/infiltratorfs_rw_namespace.inc"
internal="$root/kernel/infiltratorfs_internal.h"
pagecache="$root/kernel/infiltratorfs_pagecache.c"
read_cache="$root/kernel/infiltratorfs_read_cache.c"
core="$root/kernel/infiltratorfs_core.c"
linux_meta="$root/kernel/infiltratorfs_linux_meta.inc"
rw="$root/kernel/infiltratorfs_rw.inc"

# Sequential EOF appends must stay at the checksum tail rather than collecting
# the historical checksum chain on every group boundary.
grep -Fq 'infilfs_native_checksum_append_tail' "$data"
grep -Fq 'infilfs_native_writer_tail_lookup' "$data"
grep -Fq 'infilfs_native_writer_tail_store' "$data"
grep -Fq 'start_block == old_blocks' "$data"

append_body="$(sed -n '/static int infilfs_native_checksum_append_tail(/,/^}/p' "$data")"
! grep -Fq 'infilfs_native_checksum_collect' <<<"$append_body"

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

# Publication must retain the two-barrier dependency ordering: one barrier
# after staging data/metadata/allocation-tree blocks and one after issuing all
# three checkpoint replicas. Reintroducing the old four/five barrier sequence
# causes severe burst/stall behaviour on removable flash media.
legacy="$root/kernel/infiltratorfs_rw_legacy.inc"
commit_body="$(sed -n '/static int infilfs_rw_tx_commit(/,/^}/p' "$legacy")"
test "$(grep -Fc 'sync_blockdev(tx->sb->s_bdev)' <<<"$commit_body")" -eq 2
grep -Fq 'infilfs_rw_allocation_map_publish(tx, &next_allocation)' <<<"$commit_body"
grep -Fq 'for (n = 1; n < INFILFS_CHECKPOINT_COUNT; ++n)' <<<"$commit_body"

# Deferred publication must react to excess physical CoW churn as well as
# logical user bytes so tiny partial writes cannot consume the volume before
# reaching the nominal logical threshold. The pending transaction state is a
# shared kernel object now, so the state field belongs in the private header;
# behaviour remains owned by the native data/namespace paths.
grep -Fq 'u64 pending_physical_bytes;' "$internal"
grep -Fq 'infilfs_native_pending_should_publish' "$data"
grep -Fq 'max_excess_churn = 64ULL * 1024ULL * 1024ULL' "$data"
grep -Fq 'infilfs_native_pending_should_publish(pending)' "$ns"

# VM background writeback stages dirty folios into the same deferred
# transaction. It must not turn every writeback pass into a device-wide
# checkpoint publication; fsync/syncfs/unmount retain the durability boundary.
writepages="$(sed -n '/static int infilfs_writepages(/,/^}/p' "$pagecache")"
! grep -Fq 'infilfs_native_pending_flush_sb' <<<"$writepages"
fsync_body="$(sed -n '/static int infilfs_file_fsync(/,/^}/p' "$data")"
grep -Fq 'file_write_and_wait_range' <<<"$fsync_body"
grep -Fq 'infilfs_native_pending_flush_sb' <<<"$fsync_body"

# Verified reads must queue a bounded contiguous extent window before the
# synchronous 4 KiB integrity reader waits on the first buffer.
grep -Fq '#define INFILFS_NATIVE_READAHEAD_BLOCKS 256u' "$read_cache"
readahead_body="$(sed -n '/static void infilfs_native_readahead_extent(/,/^}/p' "$read_cache")"
grep -Fq 'sb_breadahead' <<<"$readahead_body"
grep -Fq 'INFILFS_NATIVE_READAHEAD_BLOCKS' <<<"$readahead_body"

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
