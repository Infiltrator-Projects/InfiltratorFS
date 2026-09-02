#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
data="$root/kernel/infiltratorfs_rw_data.inc"
ns="$root/kernel/infiltratorfs_rw_namespace.inc"

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
# reaching the nominal logical threshold.
grep -Fq 'u64 pending_physical_bytes;' "$data"
grep -Fq 'infilfs_native_pending_should_publish' "$data"
grep -Fq 'max_excess_churn = 64ULL * 1024ULL * 1024ULL' "$data"
grep -Fq 'infilfs_native_pending_should_publish(pending)' "$ns"

printf 'Native sequential-write scaling policy guard passed.\n'
