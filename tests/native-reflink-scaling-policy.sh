#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
reflink="$root/kernel/infiltratorfs_defrag.inc"
ownership="$root/kernel/infiltratorfs_shared_ownership.c"

fail() { echo "native reflink scaling policy: $*" >&2; exit 1; }

grep -Fq '#define INFILFS_REFLINK_INDEX_BATCH 1024u' "$reflink" ||
    fail 'bounded checksum-index batch missing'
grep -Fq 'max_nodes = DIV_ROUND_UP_ULL(' "$reflink" ||
    fail 'checksum-chain validation is not derived from logical file size'
grep -Fq 'INFILFS_NATIVE_CHECKSUMS_PER_OBJECT' "$reflink" ||
    fail 'checksum-chain bound is not tied to format checksum density'
grep -Fq 'batch_count == batch_capacity' "$reflink" ||
    fail 'checksum clone does not publish bounded index batches'
! grep -Fq 'guard < 1048576u' "$reflink" ||
    fail 'legacy ~492 GiB checksum-chain ceiling returned'
! grep -Fq 'struct infilfs_reflink_checksum_clone' "$reflink" ||
    fail 'reflink still materializes the complete checksum chain in memory'

grep -Fq 'static int infilfs_native_reflink_range(' "$reflink" ||
    fail 'native block-range reflink implementation missing'
grep -Fq 'infilfs_reflink_extract_range(' "$reflink" ||
    fail 'range reflink does not extract source extent ranges'
grep -Fq 'infilfs_reflink_splice_range(' "$reflink" ||
    fail 'range reflink does not splice destination extent ranges'
grep -Fq 'infilfs_native_checksum_set_range(' "$reflink" ||
    fail 'range reflink does not project source integrity metadata'
grep -Fq 'infilfs_quota_reserve_inode(' "$reflink" ||
    fail 'reflink/remap bypasses quota reservation'
grep -Fq 'infilfs_file_remap_file_range(' "$reflink" ||
    fail 'FICLONE is not routed through the common remap path'
! grep -Fq 'pos_in != 0 || pos_out != 0' "$reflink" ||
    fail 'legacy whole-file-only remap restriction returned'
! grep -Fq 'len != source_size' "$reflink" ||
    fail 'legacy whole-file-only length restriction returned'

python3 - "$reflink" "$ownership" <<'PY'
from pathlib import Path
import sys

reflink = Path(sys.argv[1]).read_text()
ownership = Path(sys.argv[2]).read_text()

start = reflink.index('static int infilfs_reflink_clone_checksum_chain(')
end = reflink.index('\nstatic int infilfs_native_reflink_full(', start)
body = reflink[start:end]
required = [
    'logical_blocks = DIV_ROUND_UP_ULL(',
    'max_nodes = DIV_ROUND_UP_ULL(',
    'visited >= max_nodes',
    'infilfs_native_index_update(',
]
for token in required:
    if token not in body:
        raise SystemExit(f'missing reflink checksum scaling invariant: {token}')
if 'kvmalloc_array(count' in body or 'nodes[count]' in body:
    raise SystemExit('checksum cloning regressed to whole-chain memory growth')

add_start = ownership.index('int infilfs_shared_ownership_add_owner(')
drop_start = ownership.index('int infilfs_shared_ownership_drop_owner(', add_start)
add = ownership[add_start:drop_start]
if 'infilfs_ns_index_snapshot' in add:
    raise SystemExit('incremental reflink ownership update regressed to namespace scan')
if 'shared->refs + 1u' not in add or 'refs = 2u' not in add:
    raise SystemExit('incremental reflink ownership multiplicity handling incomplete')
PY

echo 'Native reflink scaling policy guard passed.'
