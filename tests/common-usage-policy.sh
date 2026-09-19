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
optimize="$root/tools/infilfs-optimize.c"
compression="$root/tools/infilfs-compression.c"
compression_core="$root/src/volume/compression.inc"
attributes="$root/src/volume/attributes.inc"
directory_tree="$root/src/volume/directory-tree.inc"
paged_metadata="$root/src/volume/paged-metadata.inc"

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
grep -Fq 'infiltratr_u64_add_checked' "$compression_core"
grep -Fq 'infiltratr_u64_multiply_checked' "$compression_core"
grep -Fq 'infiltratr_size_multiply_checked' "$compression_core"
grep -Fq 'infiltratr_u64_multiply_checked' "$attributes"
grep -Fq 'infiltratr_size_add_checked((size_t)bytes, rec' "$directory_tree"
grep -Fq 'infiltratr_u64_add_checked' "$paged_metadata"

# Windows buffer growth and fixed-GiB presentation use Common while preserving
# the existing two-decimal GiB user-visible contract.
grep -Fq 'infiltratr_size_multiply_checked' "$win"
grep -Fq 'infiltratr_size_multiply_checked' "$winio"
grep -Fq 'infiltratr_size_multiply_checked' "$winpart"
grep -Fq 'infiltratr_format_scaled_quantity' "$win"
grep -Fq 'options.minimum_unit = 3u' "$win"
grep -Fq 'options.maximum_unit = 3u' "$win"
grep -Fq 'options.decimal_places = 2u' "$win"

# Fixed-unit command output is a product contract. Use Common's generic scaler
# rather than the auto-scaling convenience function when the command promises
# a specific unit.
grep -Fq 'infiltratr_format_scaled_quantity' "$optimize"
grep -Fq 'options.minimum_unit = 2u' "$optimize"
grep -Fq 'options.maximum_unit = 2u' "$optimize"
grep -Fq 'options.decimal_places = 2u' "$optimize"
! grep -Fq 'infiltratr_format_disk_capacity' "$optimize"
grep -Fq 'infiltratr_format_scaled_quantity' "$compression"
grep -Fq 'options.minimum_unit = 3u' "$compression"
grep -Fq 'options.maximum_unit = 3u' "$compression"
grep -Fq 'options.decimal_places = 2u' "$compression"
! grep -Fq 'infiltratr_format_disk_capacity' "$compression"

! grep -Fq 'UINT64_MAX - blocks' "$paged"
! grep -Fq '1024.0 * 1024.0 * 1024.0' "$win"
! grep -Fq 'uint64_t end = start + infs_le32_to_cpu' "$compression_core"
! grep -Fq 'physical * INFS_BLOCK_SIZE' "$compression_core"
! grep -Fq 'allocated_blocks * INFS_BLOCK_SIZE' "$attributes"
! grep -Fq 'UINT32_MAX - total_count < count' "$paged_metadata"

echo "common-usage-policy: PASS"
