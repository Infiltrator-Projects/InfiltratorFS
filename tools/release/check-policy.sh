#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
cd "$root"

fail() {
    echo "release policy: $*" >&2
    exit 1
}

state="$(mktemp)"
trap 'rm -f "$state"' EXIT
bash tools/release/resolve-request.sh . "$state" >/dev/null
# Generated only by resolve-request.sh; values are booleans, semvers and SHAs.
# shellcheck disable=SC1090
source "$state"

visible_readme_version="$(sed -nE \
    's/.*\*\*Current source:\*\*[[:space:]]*([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' \
    README.md | head -n1)"
[[ "$visible_readme_version" == "$version" ]] || \
    fail "README version ${visible_readme_version:-missing} does not match source $version"

grep -Fq "## ${version} —" CHANGELOG.md || \
    fail "CHANGELOG has no entry for source version $version"

# CMake is the semantic version authority. Windows INF syntax requires a numeric
# DriverVer mirror, but it is not allowed to become an independent version.
inf_version="$(sed -nE \
    's/^DriverVer=[^,]+,([0-9]+\.[0-9]+\.[0-9]+)\.[0-9]+$/\1/p' \
    windows-driver/infiltratorfs-native.inf | head -n1)"
[[ "$inf_version" == "$version" ]] || \
    fail "Windows INF version ${inf_version:-missing} does not match source $version"

publisher=.github/workflows/release-packages.yml
[[ -f "$publisher" ]] || fail 'release publisher workflow is missing'
grep -Fq '  workflow_run:' "$publisher" || \
    fail 'release publisher must be driven by prerequisite workflow completion'
if grep -Eq '^  (workflow_dispatch|pull_request|schedule|push):' "$publisher"; then
    fail 'release publisher must not have a direct publication trigger'
fi
if grep -Eq -- '--clobber|gh release edit' "$publisher"; then
    fail 'published releases must never be modified in place'
fi

# A semantic version may identify exactly one source revision. This check is
# unconditional: after a version has been published, any later source change
# that keeps the same CMake version must fail immediately and require a bump.
tag="v${version}"
tag_refs="$(git ls-remote --tags origin "refs/tags/${tag}" "refs/tags/${tag}^{}")"
if [[ -n "$tag_refs" ]]; then
    tag_commit="$(awk '$2 ~ /\^\{\}$/ { print $1 }' <<<"$tag_refs" | tail -n1)"
    if [[ -z "$tag_commit" ]]; then
        tag_commit="$(awk 'NR == 1 { print $1 }' <<<"$tag_refs")"
    fi
    [[ "$tag_commit" == "$sha" ]] || \
        fail "source version $version is already published at $tag_commit; current SHA is $sha and must advance the version"
fi

printf 'Release policy: PASS (%s, requested=%s)\n' "$version" "$requested"
