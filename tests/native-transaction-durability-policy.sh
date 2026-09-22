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
! grep -Fq 'sync_blockdev(tx->sb->s_bdev)' <<<"$commit_body"
grep -Fq 'infilfs_rw_allocation_map_publish(tx, &next_allocation)' <<<"$commit_body"
grep -Fq 'infilfs_rw_sync_transaction_dependencies(tx)' <<<"$commit_body"
grep -Fq 'infilfs_rw_write_block_sync(tx->sb' <<<"$commit_body"
grep -Fq 'blkdev_issue_flush(tx->sb->s_bdev)' <<<"$commit_body"
test "$(grep -Fc 'blkdev_issue_flush(tx->sb->s_bdev)' <<<"$commit_body")" -eq 2
grep -Fq 'for (n = 1; n < INFILFS_CHECKPOINT_COUNT; ++n)' <<<"$commit_body"
python3 - "$rw" <<'PY'
from pathlib import Path
import sys

s = Path(sys.argv[1]).read_text()
start = s.index('static int infilfs_rw_tx_commit(')
end = s.index('\nstatic ', start + 1)
body = s[start:end]
dependencies = body.index('infilfs_rw_sync_transaction_dependencies(tx)')
first_flush = body.index('blkdev_issue_flush(tx->sb->s_bdev)', dependencies)
checkpoint = body.index('infilfs_rw_write_block_sync(tx->sb', first_flush)
second_flush = body.index('blkdev_issue_flush(tx->sb->s_bdev)', checkpoint)
if not (dependencies < first_flush < checkpoint < second_flush):
    raise SystemExit('CoW dependencies are not durable before checkpoint publication')
PY
dependency_sync="$(sed -n '/static int infilfs_rw_sync_transaction_dependencies(/,/^}/p' "$rw")"
grep -Fq 'tx->allocated' <<<"$dependency_sync"
grep -Fq 'sb_find_get_block' <<<"$dependency_sync"

# Transaction durability, synchronization ownership and source-tree hygiene are
# one contract: automatic native qualification rejects composition growth and
# any generated Kbuild output accidentally committed beside the driver source.
bash "$root/tests/native-kernel-maintainability-policy.sh" "$root"
bash "$root/tests/native-kernel-repository-hygiene-policy.sh" "$root"

printf 'Native transaction durability policy guard passed.\n'
