#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
destination="${1:?DKMS destination directory required}"
manifest="$root/packaging/kernel-dkms-files.list"

[[ -f "$manifest" ]] || { echo "DKMS source manifest is missing: $manifest" >&2; exit 1; }
mkdir -p "$destination"
declare -A targets=()
while IFS= read -r source; do
    [[ -n "$source" && "$source" != \#* ]] || continue
    # Entries are repository paths, not arbitrary target names. The stager
    # derives an identical basename and prevents traversal and collisions.
    [[ "$source" =~ ^(kernel|include/infilfs)/[A-Za-z0-9_.-]+$ &&
       "$source" != *..* ]] || {
        echo "Invalid DKMS manifest source: $source" >&2
        exit 1
    }
    target="${source##*/}"
    [[ -z "${targets[$target]+x}" ]] || {
        echo "Duplicate DKMS manifest target: $target" >&2
        exit 1
    }
    [[ -f "$root/$source" ]] || {
        echo "DKMS manifest source is missing: $source" >&2
        exit 1
    }
    targets[$target]=1
    install -m 0644 "$root/$source" "$destination/$target"
done < "$manifest"

(( ${#targets[@]} > 0 )) || { echo 'DKMS source manifest is empty.' >&2; exit 1; }
