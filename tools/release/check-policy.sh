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

readme_version="$(sed -nE 's/^\*\*Current source version:\*\* ([0-9]+\.[0-9]+\.[0-9]+) \(Format [0-9]+\.[0-9]+\)<br>$/\1/p' README.md)"
visible_readme_version="$(sed -nE 's/^\*\*Current source:\*\* ([0-9]+\.[0-9]+\.[0-9]+)<br>$/\1/p' README.md)"
for declared in "$readme_version" "$visible_readme_version"; do
    [[ "$declared" == "$version" ]] || \
        fail "README version ${declared:-missing} does not match source $version"
done

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

if [[ "$requested" == true ]]; then
    tag="v${version}"
    tag_refs="$(git ls-remote --tags origin "refs/tags/${tag}" "refs/tags/${tag}^{}")"
    if [[ -n "$tag_refs" ]]; then
        tag_commit="$(awk '$2 ~ /\^\{\}$/ { print $1 }' <<<"$tag_refs" | tail -n1)"
        if [[ -z "$tag_commit" ]]; then
            tag_commit="$(awk 'NR == 1 { print $1 }' <<<"$tag_refs")"
        fi
        [[ "$tag_commit" == "$sha" ]] || \
            fail "release $version is already tagged at $tag_commit; requested SHA is $sha"
    fi
fi

printf 'Release policy: PASS (%s, requested=%s)\n' "$version" "$requested"
