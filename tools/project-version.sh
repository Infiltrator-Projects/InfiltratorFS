#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

source_file="${1:-CMakeLists.txt}"

if [[ "$source_file" == - ]]; then
    source_text="$(cat)"
    source_name="stdin"
else
    [[ -f "$source_file" ]] || {
        echo "InfiltratorFS version source is missing: $source_file" >&2
        exit 1
    }
    source_text="$(cat "$source_file")"
    source_name="$source_file"
fi

version="$(sed -nE \
    's/^[[:space:]]*project[[:space:]]*\([[:space:]]*InfiltratorFS[[:space:]]+VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*$/\1/p' \
    <<<"$source_text" | head -n1)"

[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
    echo "Could not determine InfiltratorFS version from $source_name" >&2
    exit 1
}

printf '%s\n' "$version"
