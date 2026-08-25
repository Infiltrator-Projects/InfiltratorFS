#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mount_helper="$repo_root/tools/mount.infiltratorfs"
fsck_helper="$repo_root/tools/fsck.infiltratorfs"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

mock_mount="$tmp/mock-mount"
mock_modprobe="$tmp/mock-modprobe"
arguments="$tmp/arguments"
modprobe_arguments="$tmp/modprobe-arguments"
cat > "$mock_mount" <<'MOCK_MOUNT'
#!/usr/bin/env bash
printf '%s\n' "$@" > "$INFILFS_TEST_ARGUMENTS"
MOCK_MOUNT
cat > "$mock_modprobe" <<'MOCK_MODPROBE'
#!/usr/bin/env bash
printf '%s\n' "$@" > "$INFILFS_TEST_MODPROBE_ARGUMENTS"
MOCK_MODPROBE
chmod 0755 "$mock_mount" "$mock_modprobe"

INFILFS_MOUNT="$mock_mount" INFILFS_MODPROBE="$mock_modprobe" \
INFILFS_TEST_ARGUMENTS="$arguments" INFILFS_TEST_MODPROBE_ARGUMENTS="$modprobe_arguments" \
    "$mount_helper" '/tmp/image with spaces' '/tmp/mount point' \
    -n -v -r -o nodev,nosuid
mapfile -t observed < "$arguments"
expected=(
    '-i'
    '-t'
    'infiltratorfs'
    '-v'
    '-o'
    'nodev,nosuid,ro'
    '/tmp/image with spaces'
    '/tmp/mount point'
)
[[ "${observed[*]}" == "${expected[*]}" ]]
[[ "$(cat "$modprobe_arguments")" == "infiltratorfs" ]]
INFILFS_MOUNT=/missing/mount INFILFS_MODPROBE=/missing/modprobe \
    "$mount_helper" image mountpoint -f

if INFILFS_MOUNT="$mock_mount" INFILFS_MODPROBE="$mock_modprobe" \
    "$mount_helper" image mountpoint -x >/dev/null 2>&1; then
    echo 'system-utilities: unsupported mount option was accepted' >&2
    exit 1
fi

# A regular image must be sent to mount(8) with the loop option while still
# using the native filesystem type.
image="$tmp/native image.img"
: > "$image"
INFILFS_MOUNT="$mock_mount" INFILFS_MODPROBE="$mock_modprobe" \
INFILFS_TEST_ARGUMENTS="$arguments" INFILFS_TEST_MODPROBE_ARGUMENTS="$modprobe_arguments" \
    "$mount_helper" "$image" '/tmp/native mount' -w
mapfile -t observed < "$arguments"
expected=(-i -t infiltratorfs -o loop,rw "$image" '/tmp/native mount')
[[ "${observed[*]}" == "${expected[*]}" ]]

mock_scrub="$tmp/mock-scrub"
cat > "$mock_scrub" <<'MOCK_SCRUB'
#!/usr/bin/env bash
exit "${INFILFS_TEST_SCRUB_STATUS:?}"
MOCK_SCRUB
chmod 0755 "$mock_scrub"

INFILFS_SCRUB="$mock_scrub" INFILFS_TEST_SCRUB_STATUS=0 "$fsck_helper" -afnp image
set +e
INFILFS_SCRUB="$mock_scrub" INFILFS_TEST_SCRUB_STATUS=2 "$fsck_helper" image
corrupt_status=$?
INFILFS_SCRUB="$mock_scrub" INFILFS_TEST_SCRUB_STATUS=1 "$fsck_helper" image
operational_status=$?
"$fsck_helper" -y image >/dev/null 2>&1
usage_status=$?
set -e
[[ "$corrupt_status" == 4 ]]
[[ "$operational_status" == 8 ]]
[[ "$usage_status" == 16 ]]

echo 'system-utilities: PASS'
