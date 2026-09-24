#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Static regression guard for the native metadata batching policy.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
ns="$root/kernel/infiltratorfs_rw_namespace.inc"
rw="$root/kernel/infiltratorfs_rw.inc"
data="$root/kernel/infiltratorfs_rw_data.inc"

grep -Fq 'INFILFS_NATIVE_METADATA_PUBLISH_CHARGE' "$ns"
grep -Fq 'pending->pending_bytes += INFILFS_NATIVE_METADATA_PUBLISH_CHARGE' "$ns"
grep -Fq 'infilfs_mod_delayed_cpu_work(&pending->idle_work' "$ns"
grep -Fq 'infilfs_mod_delayed_cpu_work(&pending->idle_work' "$data"
! grep -Fq 'mod_delayed_work(system_wq, &pending->idle_work' "$ns" || \
    fail 'metadata idle publication regressed onto system_wq'
! grep -Fq 'mod_delayed_work(system_wq, &pending->idle_work' "$data" || \
    fail 'data idle publication regressed onto system_wq'
! grep -Fq 'mod_delayed_work(system_long_wq, &pending->idle_work' "$ns" || \
    fail 'metadata idle publication regressed onto bound system_long_wq'
! grep -Fq 'mod_delayed_work(system_long_wq, &pending->idle_work' "$data" || \
    fail 'data idle publication regressed onto bound system_long_wq'
# Namespace owns the metadata charge and asks the shared deferred-transaction
# policy whether that charge now crosses a publication boundary.  The actual
# threshold/churn calculation belongs to the data transaction layer.
grep -Fq 'infilfs_native_pending_should_publish(pending)' "$ns"
grep -Fq 'pending->pending_bytes >= pending->publish_threshold' "$data"
grep -Fq 'infilfs_ns_update_paged_index' "$ns"
grep -Fq 'infilfs_native_index_update(pending, native, change_count)' "$ns"
grep -Fq 'changes[c].action == INFILFS_NS_REMOVE' "$ns"
grep -Fq 'infilfs_ns_append_paged_directory' "$ns"
grep -Fq 'if (!remove_a && !remove_b && add_name)' "$ns"
grep -Fq 'pending, last_page_no, page_block, &replacement' "$ns"
grep -Fq 'infilfs_rw_collect_directory(tx, dir, old_head' "$ns"

# Paged ADD/REPOINT operations must use the incremental index updater while
# REMOVE remains on the compacting full-rebuild path.
paged_index_body="$(sed -n '/static int infilfs_ns_update_paged_index(/,/^}/p' "$ns")"
grep -Fq 'infilfs_native_index_update(pending, native, change_count)' <<<"$paged_index_body"
grep -Fq 'changes[c].action == INFILFS_NS_REMOVE' <<<"$paged_index_body"

# Pure additions to an already-paged directory must not flatten the directory.
# They may scan existing pages for uniqueness/integrity, but only the last/new
# page and directory head are allowed onto the mutation path.
paged_append_body="$(sed -n '/static int infilfs_ns_append_paged_directory(/,/^}/p' "$ns")"
grep -Fq 'infilfs_native_store_private_or_cow(' <<<"$paged_append_body"
grep -Fq 'pointers[page_count - 1u] = cpu_to_le64(replacement)' <<<"$paged_append_body"
! grep -Fq 'infilfs_rw_collect_directory' <<<"$paged_append_body"

grep -Fq 'infilfs_posix_create_object_native' "$rw"
grep -Fq 'infilfs_posix_create_native_child' "$rw"
grep -Fq 'dir, dentry, mode, INFILFS_OBJECT_FILE, id, &block' "$rw"
grep -Fq 'dir, dentry, mode, INFILFS_OBJECT_DIRECTORY, id, &block' "$rw"

# The obsolete flush-before-create/mkdir/setattr data wrappers are retired.
# Keep the data layer free of those synchronous legacy bridges.
! grep -Fq 'infilfs_rw_create_legacy' "$data"
! grep -Fq 'infilfs_rw_mkdir_legacy' "$data"
! grep -Fq 'infilfs_rw_setattr_legacy' "$data"

# The shared native child creator is the authoritative create/mkdir mutation
# path. It must join the deferred namespace transaction rather than draining it
# first or publishing a standalone generation for each created object.
create_native_body="$(sed -n '/static int infilfs_posix_create_native_named_child(/,/^}/p' "$rw")"
grep -Fq 'infilfs_ns_begin(dir->i_sb, &pending)' <<<"$create_native_body"
grep -Fq 'infilfs_ns_finish(pending, ret)' <<<"$create_native_body"
! grep -Fq 'infilfs_native_pending_flush_sb' <<<"$create_native_body"
! grep -Fq 'infilfs_rw_tx_commit' <<<"$create_native_body"

# Dormant compatibility code must not retain a whole-device fsync implementation
# that could be accidentally rewired into the VFS later.
! grep -Fq 'sync_blockdev(file_inode(file)->i_sb->s_bdev)' \
    "$root/kernel/infiltratorfs_rw_legacy.inc"

# No write-only copy of the starting superblock belongs in pending state.
! grep -Fq 'base_disk' "$root/kernel/infiltratorfs_internal.h"
! grep -Fq 'pending->base_disk' "$data"

# tx.bitmap deliberately aliases the live working bitmap. Transaction-private
# detection therefore needs an explicit unpublished-allocation set; comparing
# the two bitmap pointers/images can never identify a private block.
private_body="$(sed -n '/static bool infilfs_native_block_private(/,/^}/p' "$data")"
grep -Fq 'infilfs_visit_contains(&pending->private_blocks, block)' <<<"$private_body"
! grep -Fq '!infilfs_rw_bitmap_get(sbi->bitmap, block)' <<<"$private_body"
grep -Fq 'infilfs_native_private_blocks_note_operation(pending);' "$data"
grep -Fq 'infilfs_visit_destroy(&pending->private_blocks);' "$data"
grep -Fq 'struct infilfs_visit_set private_blocks;' "$root/kernel/infiltratorfs_internal.h"

printf 'Native deferred metadata publication policy: PASS\n'
