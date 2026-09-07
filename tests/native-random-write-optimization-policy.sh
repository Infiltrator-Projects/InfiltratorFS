#!/usr/bin/env bash
set -euo pipefail
root="${1:-.}"
data="$root/kernel/infiltratorfs_rw_data.inc"
readcache="$root/kernel/infiltratorfs_read_cache.c"
core="$root/kernel/infiltratorfs_core.c"
ns="$root/kernel/infiltratorfs_rw_namespace.inc"
internal="$root/kernel/infiltratorfs_internal.h"

grep -Fq 'infilfs_extent_page_shape_valid' "$internal"
grep -Fq 'infilfs_extent_page_next_block' "$readcache"
grep -Fq 'sizeof(*file) + sizeof(*head) + sizeof(*root_ptr)' "$core"
grep -Fq 'for (p = pages; p-- > 0;)' "$data"
grep -Fq 'return -EOPNOTSUPP;' "$data"
grep -Fq 'infilfs_extent_page_next_block(page_block)' "$ns"
! grep -Fq 'INFILFS_EXTENT_PAGE_POINTERS' "$readcache"
! grep -Fq 'INFILFS_EXTENT_PAGE_POINTERS' "$core"
! grep -Fq 'INFILFS_EXTENT_PAGE_POINTERS' "$data"
! grep -Fq 'INFILFS_EXTENT_PAGE_POINTERS' "$ns"

printf 'Native extent-chain migration policy guard passed.\n'
