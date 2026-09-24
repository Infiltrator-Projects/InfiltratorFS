#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
rw="$root/kernel/infiltratorfs_rw_legacy.inc"
data="$root/kernel/infiltratorfs_rw_data.inc"

grep -Fq 'tx->sbi->disk.generation = cpu_to_le64(tx->generation - 1u);' "$rw"
grep -Fq 'sbi->write_poisoned = true;' "$rw"
grep -Fq 'checkpoint durability indeterminate' "$rw"
grep -Fq 'sbi->checkpoint_repair_needed = true;' "$rw"
grep -Fq 'if (sbi->write_poisoned)' "$rw"

commit_body="$(sed -n '/static int infilfs_rw_tx_commit(/,/^}/p' "$rw")"
! grep -Fq 'sync_blockdev(tx->sb->s_bdev)' <<<"$commit_body"
grep -Fq 'infilfs_rw_allocation_map_publish(tx, &next_allocation)' <<<"$commit_body"
grep -Fq 'infilfs_rw_sync_transaction_dependencies(tx)' <<<"$commit_body"
grep -Fq 'infilfs_rw_write_checkpoint_replicas_sync(' <<<"$commit_body"
grep -Fq 'blkdev_issue_flush(tx->sb->s_bdev)' <<<"$commit_body"
test "$(grep -Fc 'blkdev_issue_flush(tx->sb->s_bdev)' <<<"$commit_body")" -eq 2
checkpoint_batch="$(sed -n '/static int infilfs_rw_write_checkpoint_replicas_sync(/,/^}/p' "$rw")"
grep -Fq 'blk_start_plug' <<<"$checkpoint_batch"
grep -Fq 'write_dirty_buffer(bhs[n], REQ_SYNC)' <<<"$checkpoint_batch"
grep -Fq 'wait_on_buffer(bhs[n])' <<<"$checkpoint_batch"
python3 - "$rw" <<'PY'
from pathlib import Path
import sys

s = Path(sys.argv[1]).read_text()
start = s.index('static int infilfs_rw_tx_commit(')
end = s.index('\nstatic ', start + 1)
body = s[start:end]
dependencies = body.index('infilfs_rw_sync_transaction_dependencies(tx)')
first_flush = body.index('blkdev_issue_flush(tx->sb->s_bdev)', dependencies)
checkpoint = body.index('infilfs_rw_write_checkpoint_replicas_sync(', first_flush)
second_flush = body.index('blkdev_issue_flush(tx->sb->s_bdev)', checkpoint)
if not (dependencies < first_flush < checkpoint < second_flush):
    raise SystemExit('CoW dependencies are not durable before checkpoint publication')
PY
dependency_sync="$(sed -n '/static int infilfs_rw_sync_transaction_dependencies(/,/^}/p' "$rw")"
grep -Fq 'tx->allocated' <<<"$dependency_sync"
grep -Fq 'sb_find_get_block' <<<"$dependency_sync"

# Once deferred publication fails, the mount must fail closed.  Never retry an
# active transaction whose checkpoint/durability outcome may be indeterminate;
# close/remount recovery is the only authority for selecting the generation.
python3 - "$data" <<'PY'
from pathlib import Path
import sys

s = Path(sys.argv[1]).read_text()
start = s.index('static int infilfs_native_pending_commit_locked(')
end = s.index('\nint infilfs_native_pending_flush_sb(', start)
body = s[start:end]
failed = body.index('if (pending->commit_failed)')
active = body.index('if (!pending->active)')
commit = body.index('infilfs_rw_tx_commit(&pending->tx)')
if not (failed < active < commit):
    raise SystemExit('failed deferred publication can be retried before remount')

destroy_start = s.index('static void infilfs_native_pending_destroy(')
destroy_end = s.index('\nstatic bool infilfs_native_choose_scored_extent(', destroy_start)
destroy = s[destroy_start:destroy_end]
if 'pending->active && !pending->commit_failed' not in destroy:
    raise SystemExit('unmount can retry a failed deferred publication')
PY

# Transaction durability, synchronization ownership and source-tree hygiene are
# one contract: automatic native qualification rejects composition growth and
# any generated Kbuild output accidentally committed beside the driver source.
bash "$root/tests/native-kernel-maintainability-policy.sh" "$root"
bash "$root/tests/native-kernel-repository-hygiene-policy.sh" "$root"

printf 'Native transaction durability policy guard passed.\n'
