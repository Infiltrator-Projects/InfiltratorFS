#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
kernel="$root/kernel"
driver="$kernel/infiltratorfs_core.c"
rw="$kernel/infiltratorfs_rw.inc"
makefile="$kernel/Makefile"
ioctl="$kernel/infiltratorfs_ioctl.h"
resize="$kernel/infiltratorfs_resize.c"
quota="$kernel/infiltratorfs_quota.inc"

fail() {
    echo "native kernel maintainability policy: $*" >&2
    exit 1
}

for file in "$driver" "$rw" "$makefile" "$ioctl" "$resize" "$quota"; do
    test -f "$file" || fail "missing $file"
done

# The synchronization contract must live with the shipped kernel source, not
# only in prose documentation. Keep Kbuild and the always-copied DKMS header in
# agreement about the permitted nesting directions.
for marker in \
    'resize_lock -> write_lock' \
    'quota_lock -> write_lock' \
    'write_lock -> bitmap_lock' \
    'allocation-reservation shard spinlock -> bitmap_lock'; do
    grep -Fq "$marker" "$makefile" || fail "Makefile lost lock rule: $marker"
    grep -Fq "$marker" "$ioctl" || fail "ioctl header lost lock rule: $marker"
done
grep -Fq 'A path holding write_lock must never acquire resize_lock.' "$makefile" || \
    fail 'write_lock -> resize_lock prohibition is undocumented'
grep -Fq 'Ordinary data writers must not acquire quota_lock while holding' "$makefile" || \
    fail 'write_lock -> quota_lock prohibition is undocumented'
grep -Fq 'linux_meta_lock owns compound' "$ioctl" || \
    fail 'linux_meta_lock ownership is undocumented'
grep -Fq 'write_lock is also the persistent transaction/checkpoint publication domain.' "$ioctl" || \
    fail 'transaction ownership is undocumented'

# Keep the geometry path aligned with the declared resize_lock -> write_lock
# order. The second lock must be acquired after resize_lock in the public resize
# wrapper, never the reverse.
resize_outer="$(grep -nF 'mutex_lock(&sbi->resize_lock);' "$resize" | tail -n1 | cut -d: -f1)"
resize_inner="$(awk -v start="${resize_outer:-0}" '/mutex_lock\(&sbi->write_lock\);/ && NR > start { print NR; exit }' "$resize")"
test -n "$resize_outer" || fail 'resize_lock acquisition not found'
test -n "$resize_inner" || fail 'write_lock acquisition after resize_lock not found'
(( resize_outer < resize_inner )) || fail 'resize lock order reversed'

# Quota administration has paths that intentionally enter persistent mutation
# while quota_lock is held. Verify both domains remain present; deeper call-flow
# behaviour is covered by the mounted quota qualification rather than a fragile
# text parser.
grep -Fq 'mutex_lock(&sbi->quota_lock);' "$quota" || fail 'quota_lock acquisition missing'
grep -Fq 'mutex_lock(&sbi->write_lock);' "$quota" || fail 'quota write_lock acquisition missing'


# The native driver must stay a genuine multi-object Kbuild module.  The
# allocation map is the first extracted subsystem and must never regress into
# textual inclusion.
grep -Fqx 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o infiltratorfs_resize.o infiltratorfs_index_tree.o infiltratorfs_parallel_alloc.o' "$makefile" || \
    fail 'kernel module is no longer built from explicit component objects'
test -f "$kernel/infiltratorfs_internal.h" || fail 'missing private kernel API header'
test -f "$kernel/infiltratorfs_allocation_map.c" || fail 'allocation map object missing'
test -f "$kernel/infiltratorfs_index_tree.c" || fail 'object-index tree object missing'
test -f "$kernel/infiltratorfs_parallel_alloc.c" || fail 'parallel allocator object missing'
test ! -e "$kernel/infiltratorfs_parallel_alloc.inc" || fail 'parallel allocator regressed to textual include'
! grep -Fq 'infiltratorfs_parallel_alloc.inc' "$rw" || fail 'RW compositor textually includes parallel allocator'
test ! -e "$kernel/infiltratorfs_index_tree.inc" || fail 'object-index tree regressed to textual include'
! grep -Fq 'infiltratorfs_index_tree.inc' "$driver" || fail 'core textually includes object-index tree'
test -f "$kernel/infiltratorfs_resize.c" || fail 'resize object missing'
test ! -e "$kernel/infiltratorfs_resize.inc" || fail 'resize regressed to textual include'
! grep -Fq 'infiltratorfs_resize.inc' "$driver" || fail 'core textually includes resize'
test ! -e "$kernel/infiltratorfs_allocation_map.inc" || fail 'allocation map regressed to textual include'
! grep -Fq 'infiltratorfs_allocation_map.inc' "$driver" || fail 'core textually includes allocation map'

# Only the core object and the explicit RW compositor may textually compose
# remaining implementation .inc units. A leaf .inc importing another leaf creates hidden
# ordering/cycle dependencies and is rejected.
while IFS= read -r file; do
    case "$file" in
        "$driver"|"$rw") ;;
        *) fail "unexpected nested kernel implementation include: $file" ;;
    esac
done < <(grep -RIlE '#include[[:space:]]+"infiltratorfs_[^"]+\.inc"' \
    "$kernel" --include='*.c' --include='*.inc')

# Preserve the known RW layer order. This is deliberately an ordering contract,
# not a claim that the current macro-composition mechanism should live forever.
ordered=(
    infiltratorfs_rw_legacy.inc
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
    test -n "$line" || fail "RW compositor lost $include"
    (( line > previous )) || fail "RW compositor order changed at $include"
    previous="$line"
done

# Macro-renamed entry points are migration debt. Guard the two known alias
# bridges structurally instead of counting every macro in rw.inc (which also
# contains operation-table construction macros and caused false positives).
legacy_block="$(sed -n \
    '/^#define infilfs_rw_tx_begin infilfs_rw_tx_begin_legacy$/,/^#include "infiltratorfs_rw_legacy.inc"$/p' \
    "$rw")"
legacy_aliases="$(grep -Ec '^#define infilfs_[a-z0-9_]+[[:space:]]+infilfs_[a-z0-9_]+_legacy$' <<<"$legacy_block" || true)"
test "$legacy_aliases" -eq 7 || \
    fail "legacy alias bridge changed ($legacy_aliases entries; expected 7)"

data_block="$(sed -n \
    '/^#define infilfs_rw_create __maybe_unused infilfs_rw_create_data$/,/^#include "infiltratorfs_rw_data.inc"$/p' \
    "$rw")"
data_aliases="$(grep -Ec '^#define infilfs_[a-z0-9_]+[[:space:]]+__maybe_unused[[:space:]]+infilfs_[a-z0-9_]+_data$' <<<"$data_block" || true)"
test "$data_aliases" -eq 3 || \
    fail "data alias bridge changed ($data_aliases entries; expected 3)"

grep -Fq '#define infilfs_file_read_iter infilfs_file_read_iter_cached' "$rw" || \
    fail 'read-cache alias bridge changed'
grep -Fq '#define infilfs_rw_fill_common_attributes infilfs_posix_fill_common_attributes' "$rw" || \
    fail 'POSIX attribute alias bridge changed'

# Do not let the single-TU implementation silently become larger while it is
# being retired layer-by-layer. These ceilings leave practical edit headroom
# over the current sources but force deliberate decomposition before another
# major subsystem is added to the same textual composition.
check_bytes() {
    local file="$1" limit="$2" bytes
    bytes="$(wc -c < "$file")"
    (( bytes <= limit )) || fail "$file is $bytes bytes (ceiling $limit)"
}
check_bytes "$driver" 120000
check_bytes "$rw" 90000
check_bytes "$kernel/infiltratorfs_rw_legacy.inc" 100000
check_bytes "$kernel/infiltratorfs_rw_data.inc" 220000
check_bytes "$kernel/infiltratorfs_rw_namespace.inc" 100000
check_bytes "$quota" 70000

printf 'Native kernel locking/composition maintainability policy guard passed.\n'
