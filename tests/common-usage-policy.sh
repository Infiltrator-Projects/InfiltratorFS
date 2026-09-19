#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:?source tree required}"
cmake="$root/CMakeLists.txt"
paged="$root/src/volume/paged-extents.inc"
classic="$root/src/volume/extent-replacement.inc"
allocation="$root/src/volume/allocation-map.inc"
hole="$root/src/volume/file-hole-punch.inc"
tool="$root/tools/infilfs-tool/part-02.inc"
win="$root/tools/windows/infiltratorfs-windows.c"
winio="$root/src/platform/win32_io.c"
winpart="$root/src/platform/win32_partition_io.c"

grep -Fq 'set(INFILTRATR_COMMON_REQUIRED_VERSION "1.19.8")' "$cmake"
grep -Fq '3bfcb6f76ca44ac33bc2fee54fb114caa0eca5f9' "$cmake"

# Persistent/range arithmetic uses Common's checked contracts rather than
# parallel hand-written overflow tests.
grep -Fq 'infiltratr_u64_multiply_checked' "$paged"
grep -Fq 'infiltratr_u64_add_checked' "$paged"
grep -Fq 'infiltratr_u64_add_checked' "$classic"
grep -Fq 'infiltratr_size_add_checked(leaves, branches' "$allocation"
grep -Fq 'infiltratr_size_multiply_checked' "$allocation"
grep -Fq 'infiltratr_u64_add_saturating' "$hole"
grep -Fq 'infiltratr_u64_add_checked' "$tool"

# Windows buffer growth and fixed-GiB presentation use Common while preserving
# the existing two-decimal GiB user-visible contract.
grep -Fq 'infiltratr_size_multiply_checked' "$win"
grep -Fq 'infiltratr_size_multiply_checked' "$winio"
grep -Fq 'infiltratr_size_multiply_checked' "$winpart"
grep -Fq 'infiltratr_format_scaled_quantity' "$win"
grep -Fq 'options.minimum_unit = 3u' "$win"
grep -Fq 'options.maximum_unit = 3u' "$win"
grep -Fq 'options.decimal_places = 2u' "$win"

! grep -Fq 'UINT64_MAX - blocks' "$paged"
! grep -Fq '1024.0 * 1024.0 * 1024.0' "$win"

echo "common-usage-policy: PASS"
