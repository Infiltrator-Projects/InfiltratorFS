#!/usr/bin/env bash
# Static regression guard for the native metadata batching policy.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
ns="$root/kernel/infiltratorfs_rw_namespace.inc"
rw="$root/kernel/infiltratorfs_rw.inc"

grep -Fq 'INFILFS_NATIVE_METADATA_PUBLISH_CHARGE' "$ns"
grep -Fq 'pending->pending_bytes += INFILFS_NATIVE_METADATA_PUBLISH_CHARGE' "$ns"
grep -Fq 'mod_delayed_work(system_wq, &pending->idle_work' "$ns"
grep -Fq 'pending->pending_bytes >= pending->publish_threshold' "$ns"
grep -Fq 'infilfs_ns_update_paged_index' "$ns"
grep -Fq 'infilfs_native_index_update(pending, native, change_count)' "$ns"
grep -Fq 'changes[c].action == INFILFS_NS_REMOVE' "$ns"

grep -Fq 'infilfs_posix_create_object_native' "$rw"
grep -Fq 'infilfs_posix_create_native_child' "$rw"
grep -Fq 'dir, dentry, mode, INFILFS_OBJECT_FILE, id, &block' "$rw"
grep -Fq 'dir, dentry, mode, INFILFS_OBJECT_DIRECTORY, id, &block' "$rw"

# The compatibility wrappers may remain compiled for coverage/history, but the
# final POSIX create/mkdir entry points must no longer invoke them.
create_body="$(sed -n '/static int infilfs_posix_create(/,/^}/p' "$rw")"
mkdir_body="$(sed -n '/static .*infilfs_posix_mkdir(/,/^}/p' "$rw")"
! grep -Fq 'infilfs_rw_create_data(' <<<"$create_body"
! grep -Fq 'infilfs_rw_mkdir_data(' <<<"$mkdir_body"

printf 'Native deferred metadata publication policy: PASS\n'
