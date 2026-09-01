#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
driver="$root/kernel/infiltratorfs.c"
allocator="$root/kernel/infiltratorfs_parallel_alloc.inc"
data="$root/kernel/infiltratorfs_rw_data.inc"
workflow="$root/.github/workflows/kernel-module.yml"

for file in "$driver" "$allocator" "$data" "$workflow"; do
    test -f "$file"
done

grep -Fq 'enum infilfs_media_profile' "$driver"
grep -Fq 'INFILFS_MEDIA_ROTATIONAL' "$driver"
grep -Fq 'INFILFS_MEDIA_NONROTATIONAL' "$driver"
grep -Fq 'INFILFS_MEDIA_BALANCED' "$driver"
grep -Fq 'fsparam_enum("media", Opt_media, infilfs_media_param_values)' "$driver"
grep -Fq 'vfs_parse_fs_param_source' "$driver"
grep -Fq 'blk_queue_rot(queue)' "$driver"
grep -Fq 'blk_queue_is_zoned(queue)' "$driver"
grep -Fq 'zoned block devices require zone-aware allocation' "$driver"
grep -Fq '.show_options = infilfs_show_options' "$driver"

grep -Fq 'static void infilfs_native_media_scores' "$data"
grep -Fq 'if (sbi->media_profile == INFILFS_MEDIA_ROTATIONAL)' "$data"
grep -Fq '*primary = distance;' "$data"
grep -Fq 'if (sbi->media_profile == INFILFS_MEDIA_NONROTATIONAL)' "$data"
grep -Fq 'U64_MAX - tail : slack' "$data"
grep -Fq 'tx->sbi->media_profile != INFILFS_MEDIA_ROTATIONAL' "$data"

grep -Fq 'sbi->media_profile == INFILFS_MEDIA_ROTATIONAL &' "$allocator"
grep -Fq 'hint = min_t(u64, preferred, arena_end - count);' "$allocator"
grep -Fq 'media_rotational_scored=' "$allocator"
grep -Fq 'media_nonrotational_scored=' "$allocator"
grep -Fq 'media_source=' "$allocator"

grep -Fq 'native-media-placement-qualification.sh' "$workflow"

printf 'Native media-aware placement policy guard passed.\n'
