#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
output="${2:-}"
cd "$root"

fail() {
    echo "release request identity: $*" >&2
    exit 1
}

sha="$(git rev-parse HEAD)"
version="$(bash tools/project-version.sh CMakeLists.txt)"
previous=""

if git rev-parse --verify HEAD^ >/dev/null 2>&1; then
    previous_cmake="$(mktemp)"
    trap 'rm -f "$previous_cmake"' EXIT
    if git show HEAD^:CMakeLists.txt >"$previous_cmake" 2>/dev/null; then
        previous="$(bash tools/project-version.sh "$previous_cmake")"
    fi
fi

requested=false
if [[ -n "$previous" && "$version" != "$previous" ]]; then
    requested=true
fi

emit() {
    local key="$1"
    local value="$2"
    if [[ -n "$output" ]]; then
        printf '%s=%s\n' "$key" "$value" >> "$output"
    else
        printf '%s=%s\n' "$key" "$value"
    fi
}

emit requested "$requested"
emit version "$version"
emit previous_version "$previous"
emit sha "$sha"

if [[ -n "${GITHUB_ENV:-}" ]]; then
    printf 'VERSION=%s\n' "$version" >> "$GITHUB_ENV"
fi

if [[ "$requested" == true ]]; then
    echo "Release request: $previous -> $version at $sha"
else
    echo "No release request: version remains $version at $sha"
fi
