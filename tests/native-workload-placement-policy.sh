#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
driver="$root/kernel/infiltratorfs.c"
allocator="$root/kernel/infiltratorfs_parallel_alloc.inc"
data="$root/kernel/infiltratorfs_rw_data.inc"

for file in "$driver" "$allocator" "$data"; do
    test -f "$file"
done

# The policy must distinguish streaming EOF growth, in-place/random CoW, and
# sparse growth without changing the persistent Format 0.17 representation.
grep -Fq 'enum infilfs_data_workload' "$driver"
grep -Fq 'INFILFS_DATA_WORKLOAD_SEQUENTIAL' "$driver"
grep -Fq 'INFILFS_DATA_WORKLOAD_RANDOM' "$driver"
grep -Fq 'INFILFS_DATA_WORKLOAD_SPARSE' "$driver"
grep -Fq 'infilfs_native_classify_write' "$data"
grep -Fq 'if (pos == old_size)' "$data"
grep -Fq 'if (pos > old_size)' "$data"

# Fallback placement must be scored rather than reverting directly to global
# next-fit. Sequential growth scores physical distance and contiguous tail;
# random/sparse writes use best-fit slack to preserve large streaming runs.
grep -Fq 'infilfs_native_choose_scored_extent' "$data"
grep -Fq 'primary = infilfs_native_block_distance(candidate, preferred);' "$data"
grep -Fq 'secondary = U64_MAX - (extent_end - (candidate + wanted));' "$data"
grep -Fq 'primary = extent->count - wanted;' "$data"
grep -Fq 'if (workload == INFILFS_DATA_WORKLOAD_SEQUENTIAL &' "$data"
grep -Fq 'reservation && reservation->active' "$data"
grep -Fq 'allocation_locality_scored' "$data"
grep -Fq 'allocation_best_fit' "$data"

# Random overwrites should anchor locality near the block being replaced,
# rather than always allocating at the physical tail of the file.
grep -Fq 'old_flags == INFILFS_EXTENT_NORMAL)' "$data"
grep -Fq 'preferred = old_physical;' "$data"

# Parallel pre-reservations remain enabled for streaming growth. Random/sparse
# writes deliberately enter the scored free-extent path so they cannot consume
# the first available chunk of an otherwise large contiguous run.
reserve_body="$(sed -n '/Reserve the first bounded data chunk/,/mutex_lock(&sbi->write_lock)/p' "$data")"
grep -Fq 'workload == INFILFS_DATA_WORKLOAD_SEQUENTIAL' <<<"$reserve_body"
grep -Fq 'infilfs_parallel_reserve_data' <<<"$reserve_body"

# Workload telemetry is volatile and reported at unmount for mounted evidence.
grep -Fq 'infilfs_parallel_note_workload' "$allocator"
grep -Fq 'workload_seq=' "$allocator"
grep -Fq 'workload_random=' "$allocator"
grep -Fq 'workload_sparse=' "$allocator"
grep -Fq 'locality_scored=' "$allocator"
grep -Fq 'best_fit=' "$allocator"

printf 'Native workload-aware placement policy guard passed.\n'
