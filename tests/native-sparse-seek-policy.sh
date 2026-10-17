#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
kernel="$root/kernel"
rw="$kernel/infiltratorfs_rw.inc"
pagecache="$kernel/infiltratorfs_pagecache.c"
state="$kernel/infiltratorfs_internal.h"

fail() {
    echo "native sparse-seek policy: $*" >&2
    exit 1
}

for file in "$rw" "$pagecache" "$state"; do
    test -f "$file" || fail "missing $file"
done

llseek_body="$(sed -n '/static loff_t infilfs_file_llseek(/,/^}/p' "$rw")"
helper_body="$(sed -n '/bool infilfs_pagecache_next_pending_data(/,/^}/p' "$pagecache")"

grep -Fq 'infilfs_pagecache_next_pending_data(' <<<"$llseek_body" || \
    fail 'SEEK_DATA/SEEK_HOLE no longer merges pending page-cache data'
grep -Fq 'folio_test_private_2(folio)' <<<"$helper_body" || \
    fail 'sparse seek no longer uses the pending-CoW page-cache marker'
grep -Fq 'filemap_get_folios(mapping' <<<"$helper_body" || \
    fail 'sparse seek no longer scans the live page cache'
grep -Fq 'bool infilfs_pagecache_next_pending_data(' "$state" || \
    fail 'page-cache sparse-seek helper is missing from the private kernel API'

# Sparse seek must describe the live buffered view without turning lseek(2)
# into a durability or allocation boundary.
! grep -Fq 'filemap_write_and_wait' <<<"$llseek_body" || \
    fail 'sparse seek forces buffered writeback'
! grep -Fq 'infilfs_native_pending_flush_sb' <<<"$llseek_body" || \
    fail 'sparse seek forces transaction publication'
! grep -Fq 'fsync' <<<"$llseek_body" || \
    fail 'sparse seek depends on fsync'

grep -Fq 'payload->attributes.logical_size' <<<"$llseek_body" || \
    fail 'inline sparse-seek boundary no longer uses persisted inline size'

printf 'Native sparse-seek page-cache policy guard passed.\n'
