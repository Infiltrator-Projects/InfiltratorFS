#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
kernel="$root/kernel/infiltratorfs_defrag.inc"
abi="$root/kernel/infiltratorfs_ioctl.h"
tool="$root/tools/infilfs-optimize.c"

[[ -f "$kernel" && -f "$abi" && -f "$tool" ]]

grep -Fq 'INFILFS_IOC_GET_FRAGMENTATION' "$abi"
grep -Fq 'INFILFS_IOC_DEFRAG_FILE' "$abi"
grep -Fq 'infilfs_native_alloc_data_exact' "$kernel"
grep -Fq 'infilfs_native_release_replaced_extents' "$kernel"
grep -Fq 'infilfs_native_build_file_object' "$kernel"
grep -Fq 'infilfs_native_pending_flush_sb' "$kernel"
grep -Fq 'copy_to_user' "$kernel"
grep -Fq 'copy_from_user' "$kernel"
grep -Fq -- '--defrag' "$tool"
grep -Fq -- '--metrics' "$tool"
grep -Fq -- '--recursive' "$tool"

echo 'Native fragmentation metrics/online defrag policy guard passed.'
