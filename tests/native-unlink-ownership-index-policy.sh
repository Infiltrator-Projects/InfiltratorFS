#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root="${1:-.}"
ns="$root/kernel/infiltratorfs_rw_namespace.inc"
state="$root/kernel/infiltratorfs_internal.h"
data="$root/kernel/infiltratorfs_rw_data.inc"
reflink="$root/kernel/infiltratorfs_defrag.inc"

fail() { echo "native unlink ownership index policy: $*" >&2; exit 1; }
for file in "$ns" "$state" "$data" "$reflink"; do test -f "$file" || fail "missing $file"; done

grep -Fq 'struct infilfs_native_shared_range' "$state" || fail 'shared-range state missing'
grep -Fq 'shared_range_index_valid' "$state" || fail 'shared-range validity state missing'
grep -Fq 'infilfs_ns_shared_range_index_build' "$ns" || fail 'shared-range builder missing'
grep -Fq 'infilfs_ns_shared_range_maybe_shared' "$ns" || fail 'shared-range query missing'
grep -Fq 'infilfs_ns_other_reference_cover' "$ns" || fail 'exact ownership fallback missing'
grep -Fq 'kvfree(pending->shared_ranges);' "$data" || fail 'ownership index unmount cleanup missing'
grep -Fq 'A non-inline reflink has just introduced a second live owner' "$reflink" || fail 'reflink invalidation rationale missing'

python3 - "$reflink" <<'PY'
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text()
start = s.index('static int infilfs_native_reflink_full(')
end = s.index('\nstatic loff_t infilfs_file_remap_file_range(', start)
body = s[start:end]
clone = body.find('infilfs_native_build_file_object(')
free_ranges = body.find('kvfree(pending->shared_ranges);')
clear_ptr = body.find('pending->shared_ranges = NULL;', free_ranges)
clear_count = body.find('pending->shared_range_count = 0;', clear_ptr)
invalidate = body.find('pending->shared_range_index_valid = false;', clear_count)
finish = body.find('infilfs_ns_finish(pending, ret)')
if min(clone, free_ranges, clear_ptr, clear_count, invalidate, finish) < 0:
    raise SystemExit('reflink ownership-index invalidation sequence incomplete')
if not (clone < free_ranges < clear_ptr < clear_count < invalidate < finish):
    raise SystemExit('reflink ownership index is not invalidated before publication')
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
