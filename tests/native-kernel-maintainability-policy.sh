#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
kernel="$root/kernel"
driver="$kernel/infiltratorfs.c"
rw="$kernel/infiltratorfs_rw.inc"
makefile="$kernel/Makefile"
ioctl="$kernel/infiltratorfs_ioctl.h"

for file in "$driver" "$rw" "$makefile" "$ioctl"; do
    test -f "$file"
done

# The lock/transaction contract must live with the shipped kernel source, not
# only in prose documentation. Keep the Kbuild and always-copied DKMS header in
# agreement about the permitted nesting directions.
for marker in \
    'resize_lock -> write_lock' \
    'quota_lock -> write_lock' \
    'write_lock -> bitmap_lock' \
    'allocation-reservation shard spinlock -> bitmap_lock'; do
    grep -Fq "$marker" "$makefile"
    grep -Fq "$marker" "$ioctl"
done
grep -Fq 'A path holding write_lock must never acquire resize_lock.' "$makefile"
grep -Fq 'Ordinary data writers must not acquire quota_lock while holding' "$makefile"
grep -Fq 'linux_meta_lock owns compound' "$ioctl"
grep -Fq 'write_lock is also the persistent transaction/checkpoint publication domain.' "$ioctl"

# Keep the actual high-risk acquisition sites aligned with the declared order.
resize="$kernel/infiltratorfs_resize.inc"
quota="$kernel/infiltratorfs_quota.inc"
grep -Fq 'mutex_lock(&sbi->resize_lock);' "$resize"
grep -Fq 'mutex_lock(&sbi->write_lock);' "$resize"
resize_outer="$(grep -n 'mutex_lock(&sbi->resize_lock);' "$resize" | tail -n1 | cut -d: -f1)"
resize_inner="$(awk -v start="$resize_outer" '/mutex_lock\(&sbi->write_lock\);/ && NR > start { print NR; exit }' "$resize")"
test -n "$resize_outer"
test -n "$resize_inner"
(( resize_outer < resize_inner ))

grep -Fq 'mutex_lock(&sbi->quota_lock);' "$quota"
grep -Fq 'mutex_lock(&sbi->write_lock);' "$quota"

# Only the top-level driver and the explicit RW compositor may textually compose
# implementation .inc units. A leaf .inc importing another leaf creates hidden
# ordering/cycle dependencies and is rejected.
while IFS= read -r file; do
    case "$file" in
        "$driver"|"$rw") ;;
        *)
            echo "Unexpected nested kernel implementation include: $file" >&2
            exit 1
            ;;
    esac
done < <(grep -RIlE '#include[[:space:]]+"infiltratorfs_[^"]+\.inc"' \
    "$kernel" --include='*.c' --include='*.inc')

# Preserve the known RW layer order. This is deliberately an ordering contract,
# not a claim that the current macro-composition mechanism should live forever.
ordered=(
    infiltratorfs_rw_legacy.inc
    infiltratorfs_parallel_alloc.inc
    infiltratorfs_allocation_publish.inc
    infiltratorfs_rw_data.inc
    infiltratorfs_directory_tree.inc
    infiltratorfs_rw_read_cache.inc
    infiltratorfs_rw_namespace.inc
    infiltratorfs_pagecache.inc
    infiltratorfs_linux_meta.inc
)
previous=0
for include in "${ordered[@]}"; do
    line="$(grep -nF "#include \"$include\"" "$rw" | head -n1 | cut -d: -f1)"
    test -n "$line"
    (( line > previous ))
    previous="$line"
done

# Macro-renamed entry points are existing migration debt. Permit the currently
# qualified bridge, but fail if another alias layer is added instead of using an
# explicit helper name. The broad pattern deliberately ignores operation-table
# macro continuations containing punctuation.
alias_count="$(grep -Ec '^#define infilfs_[a-z0-9_]+[[:space:]]+(__maybe_unused[[:space:]]+)?infilfs_[a-z0-9_]+$' "$rw" || true)"
if (( alias_count > 12 )); then
    echo "Kernel RW macro-alias layering grew to $alias_count entries (limit 12)." >&2
    exit 1
fi

# Do not let the single-TU implementation silently become larger while it is
# being retired layer-by-layer. These ceilings leave practical edit headroom
# over the current sources but force deliberate decomposition before another
# major subsystem is added to the same textual composition.
check_bytes() {
    local file="$1" limit="$2" bytes
    bytes="$(wc -c < "$file")"
    if (( bytes > limit )); then
        echo "Kernel composition unit $file is $bytes bytes (ceiling $limit)." >&2
        exit 1
    fi
}
check_bytes "$driver" 120000
check_bytes "$rw" 90000
check_bytes "$kernel/infiltratorfs_rw_legacy.inc" 100000
check_bytes "$kernel/infiltratorfs_rw_data.inc" 220000
check_bytes "$kernel/infiltratorfs_rw_namespace.inc" 100000
check_bytes "$quota" 70000

printf 'Native kernel locking/composition maintainability policy guard passed.\n'
