#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root="${1:-.}"
ns="$root/kernel/infiltratorfs_rw_namespace.inc"
state="$root/kernel/infiltratorfs_internal.h"
data="$root/kernel/infiltratorfs_rw_data.inc"

fail() { echo "native unlink ownership index policy: $*" >&2; exit 1; }
for file in "$ns" "$state" "$data"; do test -f "$file" || fail "missing $file"; done

grep -Fq 'struct infilfs_native_shared_range' "$state" || fail 'shared-range state missing'
grep -Fq 'shared_range_index_valid' "$state" || fail 'shared-range validity state missing'
grep -Fq 'infilfs_ns_shared_range_index_build' "$ns" || fail 'shared-range builder missing'
grep -Fq 'infilfs_ns_shared_range_maybe_shared' "$ns" || fail 'shared-range query missing'
grep -Fq 'infilfs_ns_other_reference_cover' "$ns" || fail 'exact ownership fallback missing'
grep -Fq 'kvfree(pending->shared_ranges);' "$data" || fail 'ownership index unmount cleanup missing'

python3 - "$ns" <<'PY'
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text()
start = s.index('static int infilfs_ns_free_unshared_run(')
end = s.index('\nstatic int ', start + 1)
body = s[start:end]
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
