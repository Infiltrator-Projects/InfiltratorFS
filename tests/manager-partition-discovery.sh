#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
manager="${repo_root}/tools/infiltratorfs-manager"

# Regression for the real SD-card failure: lsblk -d suppresses child
# partitions such as /dev/mmcblk0p1. The Manager must enumerate the full block
# tree and then filter candidate disk/partition rows itself.
grep -Fq 'lsblk -prno NAME,SIZE,TYPE,RM,PKNAME,TRAN' "${manager}"
if grep -Fq 'lsblk -prndo NAME,SIZE,TYPE,RM,PKNAME,TRAN' "${manager}"; then
    echo "manager-partition-discovery: lsblk -d regression detected" >&2
    exit 1
fi

# Keep the expected partition vocabulary present so mmc/nvme/sd child
# partitions cannot silently disappear from a future selector rewrite.
grep -Fq 'TYPE' "${manager}"
grep -Eq 'part|partition' "${manager}"
grep -Fq "printf 'Fixed disk\\n'" "${manager}"
grep -Fq 'TRUE "Image file" FALSE "Disk partition"' "${manager}"

helper="${repo_root}/tools/infiltratorfs-manager-helper"
grep -Fq 'validate_partition()' "${helper}"
grep -Fq 'forensic-block)' "${helper}"
if grep -Fq 'only a removable, USB, or SD-card partition may be selected' "${helper}"; then
    echo "manager-partition-discovery: fixed-partition rejection remains" >&2
    exit 1
fi

echo "manager-partition-discovery: PASS"
