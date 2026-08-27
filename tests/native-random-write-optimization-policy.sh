#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
data="$root/kernel/infiltratorfs_rw_data.inc"
legacy="$root/kernel/infiltratorfs_rw_legacy.inc"
mainc="$root/kernel/infiltratorfs.c"
ns="$root/kernel/infiltratorfs_rw_namespace.inc"

grep -Fq 'u64 data_alloc_hint;' "$mainc"
grep -Fq 'u64 metadata_alloc_hint;' "$mainc"
grep -Fq 'infilfs_rw_tx_record_alloc' "$legacy"
grep -Fq 'metadata_alloc_hint' "$legacy"
grep -Fq 'data_alloc_hint' "$data"
grep -Fq 'operation_allocated_count' "$data"
! grep -Fq 'memcpy(pending->operation_bitmap' "$data"
grep -Fq 'infilfs_native_store_extent_page' "$data"
grep -Fq 'old_page_count' "$data"
grep -Fq 'memcmp(old_page + 1, extents + copied' "$data"
grep -Fq 'Reclaim paged extent metadata' "$ns" || grep -Fq 'INFILFS_EXTENT_PAGE_POINTERS' "$ns"

printf 'Native random-write optimization policy guard passed.\n'
