#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
legacy="$root/kernel/infiltratorfs_rw_legacy.inc"
data="$root/kernel/infiltratorfs_rw_data.inc"

grep -Fq 'free_extent_index_valid' "$legacy"
grep -Fq 'infilfs_rw_free_extent_index_rebuild' "$legacy"
rebuild="$(sed -n '/static int infilfs_rw_free_extent_index_rebuild(/,/^}/p' "$legacy")"
grep -Fq 'find_next_zero_bit' <<<"$rebuild"
grep -Fq 'find_next_bit' <<<"$rebuild"
! grep -Fq 'infilfs_rw_bitmap_get(tx->bitmap, block)' <<<"$rebuild"
grep -Fq 'infilfs_rw_free_extent_choose_forward' "$legacy"
grep -Fq 'infilfs_rw_free_extent_choose_reverse' "$legacy"

metadata_alloc="$(sed -n '/^int infilfs_rw_tx_alloc(/,/^}/p' "$legacy")"
grep -Fq 'infilfs_rw_free_extent_choose_reverse' <<<"$metadata_alloc"
grep -Fq 'for (scanned = 0; scanned < total - 1u; ++scanned)' <<<"$metadata_alloc"

data_alloc="$(sed -n '/static int infilfs_native_alloc_data_exact_reserved(/,/^}/p' "$data")"
grep -Fq 'infilfs_rw_free_extent_choose_forward' <<<"$data_alloc"
grep -Fq 'for (scanned = 0; scanned < total - 1u; ++scanned)' <<<"$data_alloc"

rollback="$(sed -n '/static int infilfs_native_operation_rollback(/,/^}/p' "$data")"
grep -Fq 'infilfs_rw_free_extent_index_rebuild' <<<"$rollback"

printf 'Native free-extent index policy guard passed.\n'

grep -Fq 'infilfs_native_metadata_reserve_blocks' "$data"
grep -Fq 'infilfs_native_visible_free_blocks' "$data"
grep -Fq 'ret != -ENOSPC' "$data"
grep -Fq 'chunk / 2u' "$data"
