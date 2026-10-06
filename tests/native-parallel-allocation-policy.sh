#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
kernel="$root/kernel"
allocator="$kernel/infiltratorfs_parallel_alloc.c"
driver="$kernel/infiltratorfs_core.c"
state="$kernel/infiltratorfs_internal.h"
package="$root/packaging/build-linux-packages.sh"
workflow="$root/.github/workflows/kernel-module.yml"
source "$root/tests/native-source-locator.sh"

write_path="$(native_unique_source_regex "$kernel" \
    '^[[:space:]]*static int infilfs_native_append_chunk_locked\(')"
journal_path="$(native_unique_source_regex "$kernel" \
    '^[[:space:]]*static int infilfs_rw_tx_begin_legacy\(')"

for file in "$allocator" "$driver" "$state" "$write_path" "$journal_path" "$package" "$workflow"; do
    test -f "$file"
done

grep -Fq '#define INFILFS_ALLOCATION_RESERVATION_SHARDS 64u' "$state"
grep -Fq 'allocation_reservation_locks' "$state"
grep -Fq 'allocation_reservations' "$state"
grep -Fq 'infilfs_parallel_reserve_data' "$allocator"
grep -Fq 'infilfs_parallel_object_preferred' "$allocator"
grep -Fq 'infilfs_parallel_consume_reservation' "$allocator"
grep -Fq 'infilfs_parallel_tx_claim' "$allocator"
grep -Fq 'write_lock(&sbi->bitmap_lock)' "$allocator"
grep -Fq 'allocation_peak_active_reservations' "$allocator"
# Long CoW/rsync workloads must never require high-order physically contiguous
# growth for the allocation or deferred-free transaction journals.
! grep -Fq 'krealloc(tx->allocated' "$allocator"
grep -Fq 'kvmalloc_array(next, sizeof(*grown), GFP_NOFS)' "$allocator"
grep -Fq 'kvfree(tx->allocated)' "$allocator"
! grep -Fq 'krealloc(tx->deferred' "$journal_path"
grep -Fq 'kvfree(tx->deferred)' "$journal_path"
grep -Fq 'kvfree(tx->allocated)' "$journal_path"
! grep -R -Fq '#include "infiltratorfs_parallel_alloc.c"' \
    "$kernel" --include='*.c' --include='*.inc'
grep -Fq 'data_allocation_hint' "$write_path"
grep -Fq 'infiltratorfs_parallel_alloc.c' "$package"
grep -Fq 'parallel-allocation-ci' "$workflow"
grep -Fq 'test "$allocator_peak" -ge 2' "$workflow"

reserve_line="$(grep -n 'infilfs_parallel_reserve_data(' "$write_path" | tail -n1 | cut -d: -f1)"
lock_line="$(awk -v reserve="$reserve_line" '
    /down_write\(&sbi->write_lock\)/ && NR > reserve { print NR; exit }
' "$write_path")"
test -n "$reserve_line"
test -n "$lock_line"
(( reserve_line < lock_line ))

# The parallel allocator is one of the principal reasons the native driver has
# multiple synchronization domains, so every ordinary allocation-policy pass
# also verifies the source-level lock/composition contract and the migration
# debt budget. Reclamation remains part of the same scaling contract.
bash "$root/tests/native-kernel-maintainability-policy.sh" "$root"
bash "$root/tests/native-rw-debt-budget.sh" "$root"
bash "$root/tests/native-unlink-ownership-index-policy.sh" "$root"

printf 'Native parallel-allocation policy guard passed.\n'
