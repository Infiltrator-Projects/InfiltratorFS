#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
kernel="$root/kernel"
allocation="$kernel/infiltratorfs_allocation_map.c"
resize="$kernel/infiltratorfs_resize.c"
source "$root/tests/native-source-locator.sh"

index_path="$(native_unique_source_regex "$kernel" \
    '^[[:space:]]*static int infilfs_rw_free_extent_index_rebuild\(')"
metadata_alloc_path="$(native_unique_source_regex "$kernel" \
    '^[[:space:]]*int infilfs_rw_tx_alloc\(')"
apply_deferred_path="$(native_unique_source_regex "$kernel" \
    '^[[:space:]]*int infilfs_rw_tx_apply_deferred\(')"
data_alloc_path="$(native_unique_source_regex "$kernel" \
    '^[[:space:]]*static int infilfs_native_alloc_data_exact_reserved\(')"
rollback_path="$(native_unique_source_regex "$kernel" \
    '^[[:space:]]*static int infilfs_native_operation_rollback\(')"
enable_path="$(native_unique_source_regex "$kernel" \
    '^[[:space:]]*static int infilfs_rw_enable\(')"
reserve_path="$(native_unique_source_regex "$kernel" \
    '^[[:space:]]*u64 infilfs_native_metadata_reserve_blocks\(')"

for file in "$index_path" "$metadata_alloc_path" "$apply_deferred_path" \
            "$data_alloc_path" "$rollback_path" "$enable_path" \
            "$reserve_path" "$allocation" "$resize"; do
    test -f "$file"
done

grep -Fq 'free_extent_index_valid' "$index_path"
grep -Fq 'infilfs_rw_free_extent_index_rebuild' "$index_path"
rebuild="$(sed -n '/static int infilfs_rw_free_extent_index_rebuild(/,/^}/p' "$index_path")"
grep -Fq 'find_next_zero_bit' <<<"$rebuild"
grep -Fq 'find_next_bit' <<<"$rebuild"
! grep -Fq 'infilfs_rw_bitmap_get(tx->bitmap, block)' <<<"$rebuild"
grep -Fq 'infilfs_rw_free_extent_choose_forward' "$index_path"
grep -Fq 'infilfs_rw_free_extent_choose_reverse' "$index_path"

metadata_alloc="$(sed -n '/^int infilfs_rw_tx_alloc(/,/^}/p' "$metadata_alloc_path")"
grep -Fq 'infilfs_rw_free_extent_choose_reverse' <<<"$metadata_alloc"
grep -Fq 'for (scanned = 0; scanned < total - 1u; ++scanned)' <<<"$metadata_alloc"

data_alloc="$(sed -n '/static int infilfs_native_alloc_data_exact_reserved(/,/^}/p' "$data_alloc_path")"
grep -Fq 'infilfs_rw_free_extent_choose_forward' <<<"$data_alloc"
grep -Fq 'for (scanned = 0; scanned < total - 1u; ++scanned)' <<<"$data_alloc"

tx_begin_path="$(native_unique_source_regex "$kernel" \
    '^[[:space:]]*static int infilfs_rw_tx_begin_legacy\(')"
tx_begin="$(sed -n '/static int infilfs_rw_tx_begin_legacy(/,/^}/p' "$tx_begin_path")"
! grep -Fq 'infilfs_rw_free_extent_index_rebuild' <<<"$tx_begin"
grep -Fq 'infilfs_rw_free_extent_index_take(tx, sbi);' <<<"$tx_begin"

apply_deferred="$(sed -n '/^int infilfs_rw_tx_apply_deferred(/,/^}/p' "$apply_deferred_path")"
grep -Fq 'INFILFS_RW_DEFERRED_INCREMENTAL_BLOCKS' "$apply_deferred_path"
grep -Fq 'infilfs_rw_free_extent_index_add(' <<<"$apply_deferred"
grep -Fq 'infilfs_rw_free_extent_index_rebuild(tx)' <<<"$apply_deferred"
grep -Fq 'if (incremental)' <<<"$apply_deferred"

rollback="$(sed -n '/static int infilfs_native_operation_rollback(/,/^}/p' "$rollback_path")"
! grep -Fq 'infilfs_rw_free_extent_index_rebuild' <<<"$rollback"
grep -Fq 'infilfs_rw_free_extent_index_add(' <<<"$rollback"

rw_enable="$(sed -n '/static int infilfs_rw_enable(/,/^}/p' "$enable_path")"
grep -Fq 'infilfs_rw_free_extent_index_rebuild_mount(sb);' <<<"$rw_enable"
grep -Fq 'infilfs_rw_free_extent_index_rebuild_mount(sb);' "$resize"

# Mount validation and resize accounting must count complete bitmap words.
# Scalar per-block free-space scans add hundreds of millions of iterations on
# a 1 TiB volume before any useful filesystem work can begin.
mount_count="$(sed -n '/static u64 infilfs_allocation_bitmap_free_count(/,/^}/p' "$allocation")"
resize_count="$(sed -n '/static u64 infilfs_resize_count_free(/,/^}/p' "$resize")"
grep -Fq 'hweight_long' <<<"$mount_count"
grep -Fq 'hweight_long' <<<"$resize_count"
! grep -Fq 'for (bit = 0; bit < total; ++bit)' "$allocation"
! grep -Fq 'for (block = 0; block < total; ++block)' <<<"$resize_count"

grep -Fq 'infilfs_native_metadata_reserve_blocks' "$reserve_path"
grep -Fq 'infilfs_native_visible_free_blocks' "$reserve_path"
grep -Fq 'ret != -ENOSPC' "$reserve_path"
grep -Fq 'chunk / 2u' "$reserve_path"

printf 'Native free-extent index policy guard passed.\n'
