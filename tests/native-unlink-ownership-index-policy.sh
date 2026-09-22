#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root="${1:-.}"
ns="$root/kernel/infiltratorfs_rw_namespace.inc"
state="$root/kernel/infiltratorfs_internal.h"
data="$root/kernel/infiltratorfs_rw_data.inc"
reflink="$root/kernel/infiltratorfs_defrag.inc"
ownership="$root/kernel/infiltratorfs_shared_ownership.c"

fail() { echo "native unlink ownership index policy: $*" >&2; exit 1; }
for file in "$ns" "$state" "$data" "$reflink" "$ownership"; do test -f "$file" || fail "missing $file"; done

grep -Fq 'struct infilfs_native_shared_range' "$state" || fail 'shared-range state missing'
grep -Fq 'shared_range_index_valid' "$state" || fail 'shared-range validity state missing'
grep -Fq 'u32 refs;' "$state" || fail 'shared-range multiplicity state missing'
grep -Fq 'infilfs_ns_shared_range_index_build' "$ns" || fail 'shared-range builder missing'
grep -Fq '(u32)coverage' "$ns" || fail 'shared-range builder does not preserve exact reference multiplicity'
grep -Fq 'infilfs_shared_ownership_add_owner' "$ownership" || fail 'incremental owner-add engine missing'
grep -Fq 'shared->refs + 1u' "$ownership" || fail 'incremental owner-add does not increment multiplicity'
grep -Fq 'infilfs_shared_ownership_drop_owner' "$ownership" || fail 'incremental owner-drop engine missing'
grep -Fq 'shared->refs - 1u' "$ownership" || fail 'incremental owner-drop does not decrement multiplicity'
! grep -Fq 'infilfs_ns_index_snapshot' "$ownership" || fail 'owner-drop regressed to whole-index scan'
grep -Fq 'infilfs_ns_shared_range_maybe_shared' "$ns" || fail 'shared-range query missing'
grep -Fq 'infilfs_ns_other_reference_cover' "$ns" || fail 'exact ownership fallback missing'
grep -Fq 'kvfree(pending->shared_ranges);' "$data" || fail 'ownership index unmount cleanup missing'
grep -Fq 'A non-inline reflink introduces one additional live owner' "$reflink" || fail 'reflink incremental ownership rationale missing'

python3 - "$reflink" <<'PY'
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text()
start = s.index('static int infilfs_native_reflink_full(')
end = s.index('\nstatic loff_t infilfs_file_remap_file_range(', start)
body = s[start:end]
clone = body.find('infilfs_native_build_file_object(')
valid = body.find('pending->shared_range_index_valid')
add = body.find('infilfs_shared_ownership_add_owner(')
finish = body.find('infilfs_ns_finish(pending, ret)')
if min(clone, valid, add, finish) < 0:
    raise SystemExit('reflink incremental ownership update sequence incomplete')
if not (clone < valid < add < finish):
    raise SystemExit('reflink ownership multiplicity is not updated before publication')
prefix = body[:add]
if 'pending->shared_range_index_valid = false;' in prefix:
    raise SystemExit('successful reflink still invalidates ownership before incremental update')
PY

grep -Fq 'struct mutex shared_range_build_lock;' "$state" || fail 'ownership-index build mutex missing'
grep -Fq 'infilfs_ns_prepare_shared_range_index' "$ns" || fail 'read-side ownership-index preparation missing'

python3 - "$ns" <<'PY'
from pathlib import Path
import sys

s = Path(sys.argv[1]).read_text()

prep_start = s.index('static int infilfs_ns_prepare_shared_range_index(')
prep_end = s.index('\nstatic bool infilfs_ns_shared_range_maybe_shared(', prep_start)
prep = s[prep_start:prep_end]
if 'mutex_lock(&sbi->shared_range_build_lock);' not in prep:
    raise SystemExit('ownership-index build is not serialized')
if 'down_read(&sbi->write_lock);' not in prep or 'up_read(&sbi->write_lock);' not in prep:
    raise SystemExit('ownership-index build lost read-side topology snapshot')
if 'down_write(&sbi->write_lock);' in prep:
    raise SystemExit('ownership-index discovery regressed under writer lock')

evict_start = s.index('static int infilfs_ns_evict_unlinked_file(')
evict_end = s.index('\nstatic int ', evict_start + 1)
evict = s[evict_start:evict_end]
prepare = evict.find('infilfs_ns_prepare_shared_range_index(pending)')
begin = evict.find('infilfs_ns_begin(inode->i_sb, &pending)')
valid = evict.find('pending->shared_range_index_valid')
if min(prepare, begin, valid) < 0:
    raise SystemExit('eviction ownership-index preflight/revalidation incomplete')
if not (prepare < begin < valid):
    raise SystemExit('eviction does not prepare ownership index before writer acquisition')

delete_start = s.index('static int infilfs_ns_delete_file_resources(')
delete_end = s.index('\nstatic int ', delete_start + 1)
delete_body = s[delete_start:delete_end]
if 'infilfs_ns_evict_free_prepared_run(' not in delete_body:
    raise SystemExit('final eviction is not using prepared ownership intervals')
if 'infilfs_ns_free_unshared_run(' in delete_body:
    raise SystemExit('final eviction regressed to generic ownership scanner')

rebuild = evict.find('infilfs_ns_rebuild_index(pending, changes, change_count)')
drop = evict.find('infilfs_shared_ownership_drop_owner(pending, inode)')
finish = evict.find('infilfs_ns_finish(pending, ret)')
if min(rebuild, drop, finish) < 0 or not (rebuild < drop < finish):
    raise SystemExit('final eviction does not update shared ownership incrementally before finish')

# Failure may invalidate the volatile accelerator, but successful eviction must
# not force the next inode to rebuild the complete ownership map.
ret_block = evict.find('if (ret) {', drop)
invalidate = evict.find('pending->shared_range_index_valid = false;', drop)
if invalidate >= 0 and (ret_block < 0 or invalidate < ret_block):
    raise SystemExit('successful eviction still invalidates the shared ownership index')

free_start = s.index('static int infilfs_ns_free_unshared_run(')
free_end = s.index('\nstatic int ', free_start + 1)
body = s[free_start:free_end]
build = body.find('infilfs_ns_shared_range_index_build(pending)')
query = body.find('infilfs_ns_shared_range_maybe_shared')
fastfree = body.find('return infilfs_rw_tx_defer_free(&pending->tx, start, count)')
scan = body.find('infilfs_ns_other_reference_cover(')
if min(build, query, fastfree, scan) < 0:
    raise SystemExit('unlink ownership fast/fallback sequence incomplete')
if not (build < query < fastfree < scan):
    raise SystemExit('whole-filesystem ownership scan is not behind the fast shared-range gate')
PY

echo 'Native unlink ownership index policy guard passed.'
