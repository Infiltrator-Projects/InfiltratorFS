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

# CMake is the semantic version authority. Shell consumers must use the single
# validated resolver rather than carrying private sed/awk parsers that can drift
# from release semantics. The Windows INF remains a required numeric mirror.
for consumer in packaging/build-linux-packages.sh support/installer/bootstrap.sh; do
    grep -Fq 'tools/project-version.sh' "$consumer" || \
        fail "$consumer must use tools/project-version.sh for source identity"
    if grep -Eq 'project\(InfiltratorFS VERSION.*(sed|awk)|sed .*project\(InfiltratorFS VERSION' "$consumer"; then
        fail "$consumer contains an independent CMake version parser"
    fi
done

inf_version="$(sed -nE \
    's/^DriverVer=[^,]+,([0-9]+\.[0-9]+\.[0-9]+)\.[0-9]+$/\1/p' \
    windows-driver/infiltratorfs-native.inf | head -n1)"
[[ "$inf_version" == "$version" ]] || \
    fail "Windows INF version ${inf_version:-missing} does not match source $version"

# An unpublished semantic version remains a release candidate across every
# justified follow-up commit. The candidate workflow therefore runs on every
# main push and lets the semantic resolver cheaply skip work once the version
# is already published. Path-gating here would allow release artifacts from an
# older SHA to survive a source correction made after the version bump.
candidate=.github/workflows/release-candidate-linux.yml
[[ -f "$candidate" ]] || fail 'release candidate workflow is missing'
candidate_trigger="$(sed -n '/^on:/,/^permissions:/p' "$candidate")"
grep -Fq '  push:' <<<"$candidate_trigger" || \
    fail 'release candidate must run on main pushes'
grep -Fq '    branches: [main]' <<<"$candidate_trigger" || \
    fail 'release candidate push trigger must target main'
if grep -Eq '^[[:space:]]+(paths|paths-ignore):' <<<"$candidate_trigger"; then
    fail 'release candidate must not path-filter unpublished exact-source revisions'
fi
grep -Fq 'tools/release/resolve-request.sh' "$candidate" || \
    fail 'release candidate must delegate release identity to the semantic resolver'

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
# A superseded exact-SHA qualification may be cancelled when main advances.
# That is not a failed release: the publisher must prove the trigger SHA still
# equals current main before interpreting prerequisite conclusions. A failure
# on current main remains a hard gate failure.
grep -Fq 'git/ref/heads/main' "$publisher" || \
    fail 'release publisher does not resolve the current main SHA before gate evaluation'
grep -Fq 'Superseded release source' "$publisher" || \
    fail 'release publisher does not cleanly ignore superseded gate evaluations'

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
