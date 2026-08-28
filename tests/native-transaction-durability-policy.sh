#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
rw="$root/kernel/infiltratorfs_rw_legacy.inc"

grep -Fq 'tx->sbi->disk.generation = cpu_to_le64(tx->generation - 1u);' "$rw"
grep -Fq 'sbi->write_poisoned = true;' "$rw"
grep -Fq 'primary checkpoint durability indeterminate' "$rw"
grep -Fq 'sbi->checkpoint_repair_needed = true;' "$rw"
grep -Fq 'if (sbi->write_poisoned)' "$rw"

printf 'Native transaction durability policy guard passed.\n'
