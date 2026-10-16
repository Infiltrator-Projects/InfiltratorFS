#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
: "${EXPECTED_SHA:?}" "${VERSION:?}" "${RUNNER_TEMP:?}"
test "$(git rev-parse HEAD)" = "$EXPECTED_SHA"
test "$(bash tools/project-version.sh CMakeLists.txt)" = "$VERSION"
tag="v$VERSION"
deb="infiltratorfs_${VERSION}_amd64.deb"
installer="infiltratorfs-${VERSION}-linux-native.run"
expected_assets="$(printf '%s\n' SHA256SUMS.txt "$deb" "$installer" | sort)"
test "$(find release-assets -maxdepth 1 -type f -printf '%f\n' | sort)" = "$expected_assets"
(cd release-assets && sha256sum -c SHA256SUMS.txt)
git fetch origin --tags
if git rev-parse -q --verify "refs/tags/$tag^{commit}" >/dev/null; then
    test "$(git rev-parse "$tag^{commit}")" = "$EXPECTED_SHA"
fi
# Creation may time out after succeeding remotely. Re-read durable state before
# retrying. Public assets are compared and never uploaded or edited in place.
state="$RUNNER_TEMP/infiltratorfs-release-state"
for attempt in 1 2 3; do
    if gh release view "$tag" --json isDraft,isPrerelease,targetCommitish > "$state"; then
        break
    fi
    if gh release create "$tag" --target "$EXPECTED_SHA" --draft \
        --title "InfiltratorFS $VERSION" --generate-notes; then
        gh release view "$tag" --json isDraft,isPrerelease,targetCommitish > "$state"
        break
    fi
    test "$attempt" != 3
    sleep "$attempt"
done
test "$(jq -r .isPrerelease "$state")" = false
test "$(jq -r .targetCommitish "$state")" = "$EXPECTED_SHA"
draft="$(jq -r .isDraft "$state")"
if [[ "$draft" == true ]]; then
    gh release upload "$tag" release-assets/* --clobber
fi
verify_remote() {
    local destination
    destination="$(mktemp -d)"
    gh release download "$tag" --dir "$destination"
    diff -qr release-assets "$destination"
    rm -rf "$destination"
}
verify_remote
if [[ "$draft" == true ]]; then
    gh release edit "$tag" --draft=false --latest
fi
verify_remote
git fetch origin "refs/tags/$tag:refs/tags/$tag"
test "$(git rev-parse "$tag^{commit}")" = "$EXPECTED_SHA"
echo "Release $tag is published from exact qualified bytes for $EXPECTED_SHA."
