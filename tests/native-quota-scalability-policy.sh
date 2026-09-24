#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
quota="$root/kernel/infiltratorfs_quota.inc"

project_usage="$(
    awk '/^static int infilfs_quota_project_rule_usage_locked\(/,/^}/' "$quota"
)"
domain_usage="$(
    awk '/^static int infilfs_quota_domain_usage_locked\(/,/^}/' "$quota"
)"
rule_usage="$(
    awk '/^static int infilfs_quota_rule_usage_locked\(/,/^}/' "$quota"
)"
set_rule="$(
    awk '/^static int infilfs_quota_set_rule\(/,/^}/' "$quota"
)"
prepare_reparent="$(
    awk '/^static int infilfs_quota_prepare_reparent_locked\(/,/^}/' "$quota"
)"
object_project="$(
    awk '/^static int infilfs_quota_object_project_locked\(/,/^}/' "$quota"
)"
candidate_subjects="$(
    awk '/^static int infilfs_quota_candidate_subjects_locked\(/,/^}/' "$quota"
)"
inode_project="$(
    awk '/^static int infilfs_quota_inode_project_locked\(/,/^}/' "$quota"
)"

# Project quotas are rooted subtrees.  They must never regress to the old
# whole-volume index scan + per-object ancestry walk that stalled writeback on
# populated filesystems.
grep -Fq 'infilfs_quota_tree_usage_locked' <<<"$project_usage"
! grep -Fq 'infilfs_ns_index_snapshot' <<<"$project_usage"
grep -Fq 'infilfs_quota_tree_usage_locked' <<<"$domain_usage"
! grep -Fq 'infilfs_ns_index_snapshot' <<<"$domain_usage"

# The generic whole-index scanner is deliberately restricted to user/group
# rules.  New project rules must dispatch to the rooted-tree implementation.
grep -Fq 'wanted_type != INFILFS_QUOTA_USER' <<<"$rule_usage"
grep -Fq 'wanted_type != INFILFS_QUOTA_GROUP' <<<"$rule_usage"
! grep -Fq 'infilfs_quota_object_project_from_index_locked' <<<"$rule_usage"
grep -Fq 'request->type == INFILFS_QUOTA_PROJECT' <<<"$set_rule"
grep -Fq 'infilfs_quota_project_rule_usage_locked' <<<"$set_rule"

# Cross-project directory renames run inside the namespace writer transaction.
# Their quota preflight must likewise remain proportional to the moved subtree.
grep -Fq 'infilfs_quota_domain_usage_locked' <<<"$prepare_reparent"
! grep -Fq 'infilfs_ns_index_snapshot' <<<"$prepare_reparent"
! grep -Fq 'infilfs_quota_reparent_member_locked' "$quota"

# Normal write admission must never flatten/snapshot the whole object index.
# Only the hard-link alias branch may take a global snapshot; single-parent
# files resolve their project from the bounded parent chain.
grep -Fq 'infilfs_quota_effective_project_locked' <<<"$object_project"
grep -Fq 'links > 1u' <<<"$object_project"
grep -Fq 'infilfs_ns_index_snapshot' <<<"$object_project"
! grep -Fq 'infilfs_quota_object_project_from_index_locked' "$quota"

# User/group-only workloads must not pay project-resolution cost at all.
grep -Fq 'infilfs_quota_has_project_rule_locked' <<<"$candidate_subjects"

# Buffered write_begin may run per folio.  Once a single-parent inode's project
# has been resolved, repeat reservations must be O(1) until project topology
# changes rather than re-walking parent/index trees for every folio.
grep -Fq 'ii->quota_project_cached' <<<"$inode_project"
grep -Fq 'ii->quota_project_epoch == sbi->quota_project_epoch' <<<"$inode_project"
grep -Fq 'ii->quota_project_id' <<<"$inode_project"
grep -Fq 'infilfs_quota_project_cache_bump_locked' "$quota"
grep -Fq 'quota_project_epoch' "$root/kernel/infiltratorfs_internal.h"
grep -Fq 'ii->quota_project_cached = false;' "$root/kernel/infiltratorfs_rw_namespace.inc"

echo "native quota scalability policy: PASS"
