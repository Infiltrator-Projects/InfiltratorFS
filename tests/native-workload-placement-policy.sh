#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
kernel="$root/kernel"
driver="$kernel/infiltratorfs_core.c"
state="$kernel/infiltratorfs_internal.h"
allocator="$kernel/infiltratorfs_parallel_alloc.c"
source "$root/tests/native-source-locator.sh"

classification="$(native_unique_source_regex "$kernel" \
    '^[[:space:]]*static enum infilfs_data_workload infilfs_native_classify_write\(')"
placement="$(native_unique_source_regex "$kernel" \
    '^[[:space:]]*static .*infilfs_native_choose_scored_extent\(')"
append_path="$(native_unique_source_fixed "$kernel" 'Aligned sequential appends dominate')"
stage_path="$(native_unique_source_regex "$kernel" \
    '^[[:space:]]*int infilfs_native_stage_block\(')"

for file in "$driver" "$state" "$allocator" "$classification" "$placement" "$append_path" "$stage_path"; do
    test -f "$file"
done

# The policy must distinguish streaming EOF growth, in-place/random CoW, and
# sparse growth without changing the persistent Format 0.18 representation.
grep -Fq 'enum infilfs_data_workload' "$state"
grep -Fq 'INFILFS_DATA_WORKLOAD_SEQUENTIAL' "$state"
grep -Fq 'INFILFS_DATA_WORKLOAD_RANDOM' "$state"
grep -Fq 'INFILFS_DATA_WORKLOAD_SPARSE' "$state"
grep -Fq 'infilfs_native_classify_write' "$classification"
grep -Fq 'if (pos == old_size)' "$classification"
grep -Fq 'if (pos > old_size)' "$classification"

# Fallback placement must be scored rather than reverting directly to global
# next-fit. Workload distance/slack/tail inputs remain explicit; the media
# helper decides which of those costs is primary for the current device.
grep -Fq 'infilfs_native_choose_scored_extent' "$placement"
grep -Fq 'distance = infilfs_native_block_distance(candidate, preferred);' "$placement"
grep -Fq 'slack = extent->count - wanted;' "$placement"
grep -Fq 'tail = extent_end - (candidate + wanted);' "$placement"
grep -Fq 'infilfs_native_media_scores(' "$placement"
grep -Fq 'Balanced/unknown keeps the pre-media-aware workload policy.' "$classification"
grep -Fq 'if (workload == INFILFS_DATA_WORKLOAD_SEQUENTIAL &' "$placement"
grep -Fq 'reservation && reservation->active' "$placement"
grep -Fq 'allocation_locality_scored' "$placement"
grep -Fq 'allocation_best_fit' "$placement"

# Random overwrites should anchor locality near the block being replaced,
# rather than always allocating at the physical tail of the file.
grep -Fq 'infilfs_extent_kind(old_flags) == INFILFS_EXTENT_NORMAL)' "$append_path"
grep -Fq 'preferred = old_physical;' "$append_path"

# Parallel pre-reservations remain enabled for streaming growth. Random/sparse
# writes deliberately enter the scored free-extent path so they cannot consume
# the first available chunk of an otherwise large contiguous run.
reserve_body="$(sed -n '/Aligned sequential appends dominate/,/down_write(&sbi->write_lock)/p' "$append_path")"
grep -Fq 'workload == INFILFS_DATA_WORKLOAD_SEQUENTIAL' <<<"$reserve_body"
grep -Fq 'infilfs_parallel_reserve_data' <<<"$reserve_body"
grep -Fq 'infilfs_native_prepare_append' <<<"$reserve_body"
grep -Fq 'infilfs_native_stage_block' "$stage_path"

# Workload telemetry is volatile and reported at unmount for mounted evidence.
grep -Fq 'infilfs_parallel_note_workload' "$allocator"
grep -Fq 'workload_seq=' "$allocator"
grep -Fq 'workload_random=' "$allocator"
grep -Fq 'workload_sparse=' "$allocator"
grep -Fq 'locality_scored=' "$allocator"
grep -Fq 'best_fit=' "$allocator"
grep -Fq 'prepared_append_successes=' "$allocator"
grep -Fq 'prepared_append_bytes=' "$allocator"

printf 'Native workload-aware placement policy guard passed.\n'
