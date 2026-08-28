#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root="${1:-.}"
grep -Fq 'INFS_INCOMPAT_DIRECTORY_TREE' "$root/include/infilfs/format.h"
grep -Fq 'INFS_DIRECTORY_TREE_DEPTH 32u' "$root/include/infilfs/format.h"
grep -Fq 'tree_dir_lookup' "$root/src/volume/phase3/directory-tree.inc"
grep -Fq 'tree_dir_build_subtree' "$root/src/volume/phase3/directory-tree.inc"
grep -Fq 'tree_dir_mutate_node' "$root/src/volume/phase3/directory-tree.inc"
grep -Fq 'tree_dir_snapshot' "$root/src/volume/phase3/directory-tree.inc"
grep -Fq 'directory-tree.inc' "$root/src/volume.c"
grep -Fq 'tree_dir_add' "$root/src/volume/phase3/part2-01.inc"
grep -Fq 'tree_dir_remove' "$root/src/volume/phase3/part2-02.inc"
printf 'Portable directory-tree core policy guard passed.\n'
