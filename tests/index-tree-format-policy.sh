#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
fmt="$root/include/infilfs/format.h"
kfmt="$root/kernel/infiltratorfs_format.h"
formatter="$root/src/format_volume.c"
core="$root/src/volume/phase3/index-tree.inc"
checkpoint="$root/src/volume/phase3/part3-04.inc"

grep -Fq '#define INFS_FORMAT_MINOR 16u' "$fmt"
grep -Fq '#define INFILFS_FORMAT_MINOR 16u' "$kfmt"
grep -Fq 'INFS_INCOMPAT_INDEX_TREE' "$fmt"
grep -Fq 'INFS_INCOMPAT_INDEX_TREE' "$formatter"
grep -Fq 'infs_encode_tree_object_index(' "$formatter"
grep -Fq 'tree_index_find' "$core"
grep -Fq 'tree_index_add' "$core"
grep -Fq 'tree_index_repoint' "$core"
grep -Fq 'tree_index_remove' "$core"
grep -Fq 'index_tree_feature' "$checkpoint"

printf 'Format 0.16 object-index tree policy guard passed.\n'
