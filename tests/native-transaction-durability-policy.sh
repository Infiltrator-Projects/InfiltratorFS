#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
rw="$root/kernel/infiltratorfs_rw_legacy.inc"

grep -Fq 'tx->sbi->disk.generation = cpu_to_le64(tx->generation - 1u);' "$rw"
grep -Fq 'sbi->write_poisoned = true;' "$rw"
grep -Fq 'checkpoint durability indeterminate' "$rw"
grep -Fq 'sbi->checkpoint_repair_needed = true;' "$rw"
grep -Fq 'if (sbi->write_poisoned)' "$rw"

commit_body="$(sed -n '/static int infilfs_rw_tx_commit(/,/^}/p' "$rw")"
test "$(grep -Fc 'sync_blockdev(tx->sb->s_bdev)' <<<"$commit_body")" -eq 2
grep -Fq 'infilfs_rw_allocation_map_publish(tx, &next_allocation)' <<<"$commit_body"
grep -Fq 'for (n = 1; n < INFILFS_CHECKPOINT_COUNT; ++n)' <<<"$commit_body"

printf 'Native transaction durability policy guard passed.\n'
