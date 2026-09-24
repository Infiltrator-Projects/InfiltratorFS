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

echo "native quota scalability policy: PASS"
