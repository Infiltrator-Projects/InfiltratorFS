#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root="${1:-.}"

grep -Fq 'INFILFS_QUOTA_USER' "$root/kernel/infiltratorfs_ioctl.h"
grep -Fq 'INFILFS_QUOTA_GROUP' "$root/kernel/infiltratorfs_ioctl.h"
grep -Fq 'INFILFS_QUOTA_PROJECT' "$root/kernel/infiltratorfs_ioctl.h"
grep -Fq 'INFILFS_IOC_SET_QUOTA' "$root/kernel/infiltratorfs_ioctl.h"
grep -Fq 'INFILFS_IOC_SET_PROJECT' "$root/kernel/infiltratorfs_ioctl.h"
grep -Fq 'INFILFS_QUOTA_DB_MAGIC' "$root/kernel/infiltratorfs_quota.inc"
grep -Fq 'infilfs_quota_reserve_inode' "$root/kernel/infiltratorfs_quota.inc"
grep -Fq 'infilfs_quota_reserve_create' "$root/kernel/infiltratorfs_quota.inc"
grep -Fq 'infilfs_quota_prepare_identity_change' "$root/kernel/infiltratorfs_quota.inc"
grep -Fq 'infilfs_quota_prepare_reparent_locked' "$root/kernel/infiltratorfs_quota.inc"
grep -Fq 'return -EDQUOT' "$root/kernel/infiltratorfs_quota.inc"
# Quota rules persist through the hidden/system Linux sidecar.  Those internal
# files must remain ordinary regular files and must never recursively consult
# sidecar metadata while linux_meta_lock already owns the compound operation.
grep -Fq 'Internal Linux sidecar files must never recursively resolve sidecar' "$root/kernel/infiltratorfs_core.c"
grep -Fq 'Internal SYSTEM sidecars are outside quota accounting' "$root/kernel/infiltratorfs_quota.inc"
grep -Fq 'reclaiming && !(ii->portable_flags & INFILFS_ATTR_SYSTEM)' "$root/kernel/infiltratorfs_core.c"
grep -Fq 'Hard-linked files intentionally have no single parent_id' "$root/kernel/infiltratorfs_quota.inc"
grep -Fq 'infilfs_quota_alias_project_from_index_locked' "$root/kernel/infiltratorfs_quota.inc"
grep -Fq '(INFILFS_ATTR_HIDDEN | INFILFS_ATTR_SYSTEM)' "$root/kernel/infiltratorfs_core.c"
grep -Fq 'infilfs_quota_reserve_inode(inode, quota_growth' "$root/kernel/infiltratorfs_rw_data.inc"
grep -Fq 'infilfs_quota_reserve_create(dir, 0, 1' "$root/kernel/infiltratorfs_rw.inc"
grep -Fq 'infilfs_quota_prepare_reparent_locked' "$root/kernel/infiltratorfs_rw_namespace.inc"
grep -Fq 'infiltratorfs-quota' "$root/CMakeLists.txt"
grep -Fq 'infiltratorfs_quota.inc' "$root/packaging/build-linux-packages.sh"
grep -Fq 'native user/group/project quota qualification: PASS' "$root/tests/native-quota-qualification.sh"
echo 'native quota policy: PASS'
