#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root="${1:-.}"
meta="$root/kernel/infiltratorfs_linux_meta.inc"
rw="$root/kernel/infiltratorfs_rw.inc"
internal="$root/kernel/infiltratorfs_internal.h"
grep -Fq 'posix_acl_from_xattr' "$meta"
grep -Fq 'posix_acl_to_xattr' "$meta"
grep -Fq 'posix_acl_create(' "$meta"
grep -Fq 'posix_acl_chmod(' "$meta"
grep -Fq 'posix_acl_update_mode' "$meta"
grep -Fq '.get_inode_acl = infilfs_posix_acl_get' "$rw"
grep -Fq '.set_acl = infilfs_posix_acl_set' "$rw"
grep -Fq 'sb->s_flags |= SB_POSIXACL' "$rw"
grep -Fq '#include <linux/posix_acl.h>' "$internal"
grep -Fq '#include <linux/posix_acl_xattr.h>' "$internal"
! grep -Fq 'INFS_IAC1_MIN_SAVINGS_DIVISOR' "$root/include/infilfs/iac1.h"
# Keep the old percentage-savings helper out of production code and all other
# regression tests. Exclude this policy file itself so the literal guard string
# does not make the negative grep self-match and fail unconditionally.
! grep -R -Fq --exclude='native-posix-acl-policy.sh' \
    'infs_iac1_savings_worthwhile' \
    "$root/kernel" "$root/src" "$root/tests" "$root/include"
