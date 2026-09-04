#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
fmt="$root/kernel/infiltratorfs_format.h"
core="$root/kernel/infiltratorfs_core.c"
data="$root/kernel/infiltratorfs_rw_data.inc"
ns="$root/kernel/infiltratorfs_rw_namespace.inc"
tree="$root/kernel/infiltratorfs_index_tree.c"

grep -Fq '#define INFILFS_OBJECT_VERSION_TREE 3u' "$fmt"
grep -Fq 'INFILFS_INCOMPAT_INDEX_TREE' "$fmt"
grep -Fq 'INFILFS_INDEX_TREE_FANOUT 256u' "$fmt"
grep -Fq 'infilfs_index_tree_lookup_head' "$tree"
grep -Fq 'infilfs_index_tree_snapshot' "$tree"
grep -Fq 'infilfs_native_index_update_tree' "$data"
grep -Fq 'infilfs_native_tree_mutate_node' "$data"
grep -Fq 'infilfs_ns_update_tree_index' "$ns"
grep -Fq 'INFILFS_OBJECT_VERSION_TREE' "$core"

printf 'Native object-index tree policy guard passed.\n'
