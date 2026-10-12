#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
output="${2:-}"
cd "$root"

sha="$(git rev-parse HEAD)"
version="$(bash tools/project-version.sh CMakeLists.txt)"
previous=""

# A release is needed while the source version is newer than the newest
# published semantic-version tag reachable from this source. This remains true
# across merge commits, squash merges, rebases and multi-commit pushes. Shallow
# CI checkouts may not contain tags, so fall back to the immediate parent only
# for policy/testing contexts; release workflows use full history and tags.
latest_tag="$(git tag --merged HEAD --list 'v*' --sort=-version:refname 2>/dev/null |
    grep -Em1 '^v[0-9]+\.[0-9]+\.[0-9]+$' || true)"
if [[ -n "$latest_tag" ]]; then
    previous="${latest_tag#v}"
elif git rev-parse --verify HEAD^ >/dev/null 2>&1; then
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
    echo "Release request: published $previous -> source $version at $sha"
else
    echo "No release request: source $version matches published/fallback $previous at $sha"
fi
