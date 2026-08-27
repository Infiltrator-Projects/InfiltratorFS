#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
data="$root/kernel/infiltratorfs_rw_data.inc"
ns="$root/kernel/infiltratorfs_rw_namespace.inc"

# Inline writes must remain in the shared deferred transaction rather than
# forcing a full publication and synchronous legacy transaction per tiny file.
grep -Fq 'infilfs_native_inline_write_iter' "$data"
dispatch="$(sed -n '/if ((u64)pos <= INFILFS_INLINE_DATA_MAX/,/^[[:space:]]*}/p' "$data" | head -n 12)"
grep -Fq 'infilfs_native_inline_write_iter' <<<"$dispatch"
! grep -Fq 'infilfs_native_pending_flush_sb' <<<"$dispatch"
! grep -Fq 'infilfs_file_write_iter_legacy' <<<"$dispatch"

# Pure paged-directory additions must use the exact volatile name locator and
# must not rescan every historical directory page on each create.
grep -Fq 'infilfs_ns_directory_locator_build' "$ns"
append="$(sed -n '/static int infilfs_ns_append_paged_directory(/,/^}/p' "$ns")"
grep -Fq 'infilfs_native_directory_locator_matches' <<<"$append"
grep -Fq 'infilfs_native_directory_locator_lookup' <<<"$append"
grep -Fq 'infilfs_native_directory_locator_insert' <<<"$append"
! grep -Fq 'for (p = 0; p < page_count; ++p)' <<<"$append"

# Rollback must discard any optimistic volatile directory state.
rollback="$(sed -n '/static int infilfs_native_operation_rollback(/,/^}/p' "$data")"
grep -Fq 'infilfs_native_directory_locator_invalidate' <<<"$rollback"

printf 'Native small-file scaling policy guard passed.\n'
