#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
allocator="$root/kernel/infiltratorfs_parallel_alloc.inc"
driver="$root/kernel/infiltratorfs.c"
rw="$root/kernel/infiltratorfs_rw.inc"
data="$root/kernel/infiltratorfs_rw_data.inc"
package="$root/packaging/build-linux-packages.sh"

for file in "$allocator" "$driver" "$rw" "$data" "$package"; do
    test -f "$file"
done

grep -Fq '#define INFILFS_ALLOCATION_RESERVATION_SHARDS 64u' "$driver"
grep -Fq 'allocation_reservation_locks' "$driver"
grep -Fq 'allocation_reservations' "$driver"
grep -Fq 'infilfs_parallel_reserve_data' "$allocator"
grep -Fq 'infilfs_parallel_object_preferred' "$allocator"
grep -Fq 'infilfs_parallel_consume_reservation' "$allocator"
grep -Fq 'infilfs_parallel_tx_claim' "$allocator"
grep -Fq 'write_lock(&sbi->bitmap_lock)' "$allocator"
grep -Fq 'allocation_peak_active_reservations' "$allocator"
grep -Fq '#include "infiltratorfs_parallel_alloc.inc"' "$rw"
grep -Fq 'data_allocation_hint' "$data"
grep -Fq 'infiltratorfs_parallel_alloc.inc' "$package"

reserve_line="$(grep -n 'infilfs_parallel_reserve_data(' "$data" | tail -n1 | cut -d: -f1)"
lock_line="$(awk -v reserve="$reserve_line" '
    /mutex_lock\(&sbi->write_lock\)/ && NR > reserve { print NR; exit }
' "$data")"
test -n "$reserve_line"
test -n "$lock_line"
(( reserve_line < lock_line ))

printf 'Native parallel-allocation policy guard passed.\n'
