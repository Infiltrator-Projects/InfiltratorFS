#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
destination="${1:?DKMS destination directory required}"
manifest="$root/packaging/kernel-dkms-files.list"

[[ -f "$manifest" ]] || { echo "DKMS source manifest is missing: $manifest" >&2; exit 1; }
mkdir -p "$destination"
declare -A targets=()
while IFS=$'\t' read -r source target; do
    [[ -n "$source" ]] || continue
    [[ "$source" == \#* ]] && continue
    [[ -n "$target" && "$target" != */* ]] || {
        echo "Invalid DKMS manifest target for $source: ${target:-missing}" >&2
        exit 1
    }
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
