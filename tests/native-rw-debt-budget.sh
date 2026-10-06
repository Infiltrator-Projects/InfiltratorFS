#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
kernel="$root/kernel"

fail() {
    echo "native RW debt budget: $*" >&2
    exit 1
}

check_bytes() {
    local relative="$1"
    local limit="$2"
    local file="$root/$relative"
    local bytes

    test -f "$file" || fail "missing $relative"
    bytes="$(wc -c < "$file")"
    (( bytes <= limit )) ||
        fail "$relative grew to $bytes bytes (debt ceiling $limit); extract responsibility before adding more"
}

# These are intentionally tight anti-growth budgets, not target sizes.  The
# files are already migration debt.  Ordinary bug fixes retain a small amount
# of headroom, while any meaningful feature growth must first move an existing
# responsibility behind a compiled component boundary.
check_bytes kernel/infiltratorfs_core.c                  115000
check_bytes kernel/infiltratorfs_rw.inc                   83000
check_bytes kernel/infiltratorfs_rw_legacy.inc            74500
check_bytes kernel/infiltratorfs_rw_data.inc             166500
check_bytes kernel/infiltratorfs_rw_namespace.inc          87500
check_bytes kernel/infiltratorfs_quota.inc                 68100
check_bytes kernel/infiltratorfs_defrag.inc                56000
check_bytes kernel/infiltratorfs_linux_meta.inc            47500

# The textual implementation surface itself may only shrink.  Adding another
# .inc implementation unit is not decomposition; new subsystems belong in a
# normal Kbuild .c object with an explicit private API.
inc_count="$(find "$kernel" -maxdepth 1 -type f -name '*.inc' | wc -l)"
(( inc_count <= 10 )) ||
    fail "kernel textual implementation count grew to $inc_count (ceiling 10)"

rw_includes="$(grep -Ec '^#include "infiltratorfs_[^"]+\.inc"$' \
    "$kernel/infiltratorfs_rw.inc" || true)"
(( rw_includes <= 5 )) ||
    fail "RW compositor gained another textual layer ($rw_includes, ceiling 5)"

core_includes="$(grep -Ec '^#include "infiltratorfs_[^"]+\.inc"$' \
    "$kernel/infiltratorfs_core.c" || true)"
(( core_includes <= 3 )) ||
    fail "core gained another textual implementation include ($core_includes, ceiling 3)"

# There must be no second compositor hiding in a leaf unit.  core.c and rw.inc
# are the only temporary owners allowed to include implementation .inc files.
while IFS= read -r file; do
    case "$file" in
        "$kernel/infiltratorfs_core.c"|"$kernel/infiltratorfs_rw.inc") ;;
        *) fail "new nested textual dependency: ${file#$root/}" ;;
    esac
done < <(grep -RIlE '#include[[:space:]]+"infiltratorfs_[^"]+\.inc"' \
    "$kernel" --include='*.c' --include='*.inc')

printf 'Native RW migration-debt budget guard passed.\n'
