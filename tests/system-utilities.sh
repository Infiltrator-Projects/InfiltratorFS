#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mount_helper="$repo_root/tools/mount.infiltratorfs"
fsck_helper="$repo_root/tools/fsck.infiltratorfs"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

mock_fuse="$tmp/mock-fuse"
arguments="$tmp/arguments"
cat > "$mock_fuse" <<'MOCK_FUSE'
#!/usr/bin/env bash
printf '%s\n' "$@" > "$INFILFS_TEST_ARGUMENTS"
MOCK_FUSE
chmod 0755 "$mock_fuse"

INFILFS_FUSE="$mock_fuse" INFILFS_TEST_ARGUMENTS="$arguments" \
    "$mount_helper" '/tmp/image with spaces' '/tmp/mount point' \
    -n -v -r -o nodev,nosuid
mapfile -t observed < "$arguments"
expected=(
    '/tmp/image with spaces'
    '/tmp/mount point'
    '-o'
    'ro'
    '-o'
    'nodev,nosuid'
)
[[ "${observed[*]}" == "${expected[*]}" ]]
INFILFS_FUSE=/missing/fuse "$mount_helper" image mountpoint -f

if INFILFS_FUSE="$mock_fuse" "$mount_helper" image mountpoint -x \
    >/dev/null 2>&1; then
    echo 'system-utilities: unsupported mount option was accepted' >&2
    exit 1
fi

mock_scrub="$tmp/mock-scrub"
cat > "$mock_scrub" <<'MOCK_SCRUB'
#!/usr/bin/env bash
exit "${INFILFS_TEST_SCRUB_STATUS:?}"
MOCK_SCRUB
chmod 0755 "$mock_scrub"

INFILFS_SCRUB="$mock_scrub" INFILFS_TEST_SCRUB_STATUS=0 \
    "$fsck_helper" -afnp image

set +e
INFILFS_SCRUB="$mock_scrub" INFILFS_TEST_SCRUB_STATUS=2 \
    "$fsck_helper" image
corrupt_status=$?
INFILFS_SCRUB="$mock_scrub" INFILFS_TEST_SCRUB_STATUS=1 \
    "$fsck_helper" image
operational_status=$?
"$fsck_helper" -y image >/dev/null 2>&1
usage_status=$?
set -e

[[ "$corrupt_status" == 4 ]]
[[ "$operational_status" == 8 ]]
[[ "$usage_status" == 16 ]]

echo 'system-utilities: PASS'
