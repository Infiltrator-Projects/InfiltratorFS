#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
kernel="$root/kernel/infiltratorfs_defrag.inc"
abi="$root/kernel/infiltratorfs_ioctl.h"
tool="$root/tools/infilfs-optimize.c"
cmake="$root/CMakeLists.txt"
package="$root/packaging/build-linux-packages.sh"

[[ -f "$kernel" && -f "$abi" && -f "$tool" && -f "$cmake" && -f "$package" ]]

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
grep -Fq 'add_executable(infilfs-optimize' "$cmake"
grep -Fq 'infilfs-optimize' "$package"
bash -n "$package"
package_entry="    'usr/bin/infilfs-optimize\$' \\"
[[ "$(grep -Fxc "$package_entry" "$package")" -eq 1 ]]

echo 'Native fragmentation metrics/online defrag policy guard passed.'
