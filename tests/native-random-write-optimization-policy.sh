#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
data="$root/kernel/infiltratorfs_rw_data.inc"
legacy="$root/kernel/infiltratorfs_rw_legacy.inc"
state="$root/kernel/infiltratorfs_internal.h"
ns="$root/kernel/infiltratorfs_rw_namespace.inc"

for file in "$data" "$legacy" "$state" "$ns"; do
    test -f "$file"
done

grep -Fq 'u64 data_alloc_hint;' "$state"
grep -Fq 'u64 metadata_alloc_hint;' "$state"
grep -Fq 'infilfs_rw_tx_record_alloc' "$legacy"
grep -Fq 'metadata_alloc_hint' "$legacy"
grep -Fq 'data_alloc_hint' "$data"
grep -Fq 'operation_allocated_count' "$data"
! grep -Fq 'memcpy(pending->operation_bitmap' "$data"
grep -Fq 'infilfs_native_store_extent_page' "$data"
grep -Fq 'old_page_count' "$data"
grep -Fq 'INFILFS_NATIVE_CHECKSUM_GROUP_CACHE_SLOTS' "$data"
grep -Fq 'infilfs_native_checksum_update_existing_group' "$data"
grep -Fq 'infilfs_native_try_single_paged_overwrite' "$data"
grep -Fq 'binary-searches the paged map' "$data"
grep -Fq 'infilfs_native_build_extent_page' "$data"
! grep -Fq 'infilfs_rw_tx_defer_free(&pending->tx, block, 1);' <(sed -n '641,760p' "$data")
grep -Fq 'memcmp(old_page + 1, extents + copied' "$data"
grep -Fq 'Reclaim paged extent metadata' "$ns" || grep -Fq 'INFILFS_EXTENT_PAGE_POINTERS' "$ns"

printf 'Native random-write optimization policy guard passed.\n'
