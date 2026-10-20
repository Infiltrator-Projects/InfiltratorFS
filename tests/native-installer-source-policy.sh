#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

# Prove release bootstrap identity and the absence of attached source/executables
# without accessing the network or modifying an installed filesystem.
installer="${1:?generated native .run path required}"
source_sha="${2:?expected complete source commit required}"
version="${3:?expected semantic release required}"
fail() { echo "native installer source policy: $*" >&2; exit 1; }

[[ -f "$installer" && -x "$installer" ]] || fail 'installer is missing'
[[ "$source_sha" =~ ^[0-9a-f]{40}$ ]] || fail 'invalid expected commit'
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail 'invalid expected version'
[[ "$(head -n1 "$installer")" == '#!/usr/bin/env bash' ]] || fail 'not an executable Bash script'
(( $(wc -c < "$installer") < 16384 )) || fail 'installer carries an unexpected payload'
bash -n "$installer"
grep -Fq "readonly SOURCE_SHA='$source_sha'" "$installer" || fail 'source not pinned'
grep -Fq "readonly RELEASE_VERSION='$version'" "$installer" || fail 'semantic version not pinned'
grep -Fq "git -C \"\$destination\" checkout --quiet --detach \"\$SOURCE_SHA\"" "$installer" || fail 'source checkout is not immutable'
grep -Fq 'submodule update --init --recursive' "$installer" || fail 'recursive gitlink checkout missing'
grep -Fq 'sha256sum --check --status' "$installer" || fail 'pinned asset verification missing'
grep -Fq 'bootstrap.sh" --build-only' "$installer" || fail 'local source build missing'
! grep -Fq '__INFILTRATORFS_NATIVE_PAYLOAD__' "$installer" || fail 'obsolete embedded archive marker'
! grep -Fq 'tail -n +"$payload_start"' "$installer" || fail 'embedded archive unpacking remains'

# Verification and dry-run must be safe even with unusable network commands.
fake="$(mktemp -d)"
trap 'rm -rf "$fake"' EXIT
for command in git curl; do
    printf '#!/bin/sh\necho "unexpected %s network invocation" >&2\nexit 89\n' "$command" > "$fake/$command"
    chmod 0755 "$fake/$command"
done
PATH="$fake:$PATH" "$installer" --verify >/dev/null
dry="$(PATH="$fake:$PATH" "$installer" --dry-run)"
grep -Fq "Pinned Git commit: $source_sha" <<<"$dry" || fail 'dry-run hides source identity'
grep -Fq 'Dry run only; no packages will be installed' <<<"$dry" || fail 'dry-run contract lost'
grep -Fq 'Native build commands:' <<<"$dry" || fail 'native build summary missing'
grep -Fq 'Native kernel module commands:' <<<"$dry" || fail 'DKMS summary missing'
printf 'Native pinned-source installer: PASS (%s)\n' "$version"
