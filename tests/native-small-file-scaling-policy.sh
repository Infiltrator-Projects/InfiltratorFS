#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
data="$root/kernel/infiltratorfs_rw_data.inc"
ns="$root/kernel/infiltratorfs_rw_namespace.inc"

# Inline writes must remain in the shared deferred transaction rather than
# forcing a full publication and synchronous legacy transaction per tiny file.
grep -Fq 'infilfs_native_inline_write_iter' "$data"
dispatch="$(sed -n '/if ((u64)pos <= INFILFS_INLINE_DATA_MAX/,/^[[:space:]]*}/p' "$data" | head -n 12)"
grep -Fq 'infilfs_native_inline_write_iter' <<<"$dispatch"
! grep -Fq 'infilfs_native_pending_flush_sb' <<<"$dispatch"
! grep -Fq 'infilfs_file_write_iter_legacy' <<<"$dispatch"


# Inline writeback preparation must not hold the filesystem-wide topology
# writer while verifying or hashing a tiny file. Snapshot the object under the
# read side, calculate the replacement digest without write_lock, then
# revalidate the complete object after acquiring the writer before publication.
grep -Fq 'static int infilfs_native_inline_snapshot(' "$data"
snapshot="$(sed -n '/static int infilfs_native_inline_snapshot(/,/^}/p' "$data")"
grep -Fq 'down_read(&sbi->write_lock);' <<<"$snapshot"
grep -Fq 'up_read(&sbi->write_lock);' <<<"$snapshot"
inline="$(sed -n '/static ssize_t infilfs_native_inline_write_iter(/,/^}/p' "$data")"
grep -Fq 'infilfs_rw_inline_digest(data, new_size, prepared_digest);' <<<"$inline"
grep -Fq 'memcmp(current_object, old_object,' <<<"$inline"
grep -Fq 'READ_ONCE(ii->object_block) != old_block' <<<"$inline"
python3 - "$data" <<'PY'
from pathlib import Path
import sys
text = Path(sys.argv[1]).read_text()
start = text.index("static ssize_t infilfs_native_inline_write_iter(")
end = text.index("\nssize_t infilfs_native_writeback_iter(", start)
body = text[start:end]
digest = body.index("infilfs_rw_inline_digest(data, new_size, prepared_digest);")
writer = body.index("down_write(&sbi->write_lock);")
revalidate = body.index("memcmp(current_object, old_object,")
if not digest < writer < revalidate:
    raise SystemExit("inline writeback no longer prepares before writer-lock revalidation")
if "infilfs_rw_load_inline(" in body:
    raise SystemExit("inline writeback regressed to integrity hashing under the writer helper")
if "(void)infilfs_native_pending_commit_locked(pending)" in body:
    raise SystemExit("inline writeback discards deferred publication failure")
publish = body.find("int publish = infilfs_native_pending_commit_locked(pending);")
error = body.find("ret = publish;", publish)
revert = body.find("goto out_revert;", error)
success = body.find("*position = pos + copied;", publish)
if min(publish, error, revert, success) < 0 or not (publish < error < revert < success):
    raise SystemExit("inline writeback does not surface publication failure before success")
PY

# Pure paged-directory additions must use the exact volatile name locator and
# must not rescan every historical directory page on each create.
grep -Fq 'infilfs_ns_directory_locator_build' "$ns"
append="$(sed -n '/static int infilfs_ns_append_paged_directory(/,/^}/p' "$ns")"
grep -Fq 'infilfs_native_directory_locator_matches' <<<"$append"
grep -Fq 'infilfs_native_directory_locator_lookup' <<<"$append"
grep -Fq 'infilfs_native_directory_locator_insert' <<<"$append"
! grep -Fq 'for (p = 0; p < page_count; ++p)' <<<"$append"

# Rollback must discard any optimistic volatile directory state.
rollback="$(sed -n '/static int infilfs_native_operation_rollback(/,/^}/p' "$data")"
grep -Fq 'infilfs_native_directory_locator_invalidate' <<<"$rollback"

printf 'Native small-file scaling policy guard passed.\n'


# Linux-only sidecar metadata must not perform an eager full-directory scan on
# the first xattr. UUID sidecar names are deterministic: cache misses use the
# directory-tree exact lookup and cache only the object actually requested.
meta="$root/kernel/infiltratorfs_linux_meta.inc"
internal="$root/kernel/infiltratorfs_internal.h"
grep -Fq 'INFILFS_LINUX_META_CACHE_BUCKETS' "$internal"
grep -Fq 'linux_meta_cache_valid' "$internal"
grep -Fq 'infilfs_linux_meta_cache_lookup' "$meta"
grep -Fq 'infilfs_tree_dir_lookup_name(' "$meta"
! grep -Fq 'infilfs_linux_meta_cache_build(' "$meta"
! grep -Fq 'infilfs_linux_meta_cache_build_visitor' "$meta"
grep -Fq 'infilfs_native_inline_write_iter' "$meta"
! grep -Fq 'infilfs_native_pending_flush_sb' "$meta"
