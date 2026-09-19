#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
check="$root/src/volume/check.inc"
scrub="$root/src/volume/scrub.inc"
ownership="$root/src/volume/ownership-validation.inc"
doc="$root/docs/FSCK-SCRUB-SEPARATION.md"

fail() { echo "fast fsck structural policy: $*" >&2; exit 1; }
for file in "$check" "$scrub" "$ownership" "$doc"; do
    test -f "$file" || fail "missing $file"
done

grep -Fq 'check_allocation_structure' "$check" || \
    fail 'fast allocation structural validator missing'
grep -Fq 'allocation_map_load(' "$check" || \
    fail 'fast fsck does not authenticate allocation-tree structure'
grep -Fq 'check_index_structure' "$check" || \
    fail 'fast object-index structural validator missing'
grep -Fq 'check_namespace_structure' "$check" || \
    fail 'fast namespace structural validator missing'
! grep -Fq 'validate_index_entries(vol)' "$check" || \
    fail 'plain fsck regressed to rereading every indexed object'
! grep -Fq 'validate_namespace_graph(vol)' "$check" || \
    fail 'plain fsck regressed to exhaustive namespace object validation'
! grep -Fq 'validate_checksum_graph(vol)' "$check" || \
    fail 'plain fsck regressed to walking every file checksum chain'
! grep -Fq 'validate_integrity_metadata(vol)' "$check" || \
    fail 'plain fsck regressed to exhaustive live ownership validation'
grep -Fq 'validate_integrity_metadata_progress(vol, progress, 0)' "$scrub" || \
    fail 'deep scrub lost exhaustive ownership validation'
grep -Fq 'validate_bitmap_ownership_progress' "$ownership" || \
    fail 'authoritative ownership scrub implementation missing'
grep -Fq 'does not reconstruct a block-by-block live ownership bitmap' "$doc" || \
    fail 'fsck/scrub ownership boundary is undocumented'

printf 'Fast fsck structural/deep scrub separation guard passed.\n'
