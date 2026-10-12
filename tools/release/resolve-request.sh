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

version="$(sed -nE 's/^project\(InfiltratorFS VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES C\)$/\1/p' CMakeLists.txt)"
[[ -n "$version" ]] || fail 'could not resolve source version from CMakeLists.txt'

mapfile -t trigger_lines < .github/release-trigger
(( ${#trigger_lines[@]} == 1 )) || fail '.github/release-trigger must contain exactly one line'
trigger="${trigger_lines[0]//[[:space:]]/}"
[[ "$trigger" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "invalid release trigger version: ${trigger:-empty}"
[[ "$trigger" == "$version" ]] || fail "release trigger $trigger does not match source version $version"

previous=""
if git rev-parse --verify HEAD^ >/dev/null 2>&1; then
    previous="$(git show HEAD^:.github/release-trigger 2>/dev/null | head -n1 | tr -d '[:space:]' || true)"
fi

requested=false
if [[ -n "$previous" && "$trigger" != "$previous" ]]; then
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

if [[ -n "${GITHUB_ENV:-}" ]]; then
    printf 'VERSION=%s\n' "$version" >> "$GITHUB_ENV"
fi

if [[ "$requested" == true ]]; then
    echo "Release request: $previous -> $version"
else
    echo "No release request: version remains $version"
fi
