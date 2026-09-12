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

mock_check="$tmp/mock-check"
mock_scrub="$tmp/mock-scrub"
check_arguments="$tmp/check-arguments"
scrub_arguments="$tmp/scrub-arguments"
cat > "$mock_check" <<'MOCK_CHECK'
#!/usr/bin/env bash
printf '%s\n' "$@" > "$INFILFS_TEST_CHECK_ARGUMENTS"
exit "${INFILFS_TEST_CHECK_STATUS:?}"
MOCK_CHECK
cat > "$mock_scrub" <<'MOCK_SCRUB'
#!/usr/bin/env bash
printf '%s\n' "$@" > "$INFILFS_TEST_SCRUB_ARGUMENTS"
exit "${INFILFS_TEST_SCRUB_STATUS:?}"
MOCK_SCRUB
chmod 0755 "$mock_check" "$mock_scrub"

# Plain fsck MUST use the fast structural checker, not scrub.
rm -f "$check_arguments" "$scrub_arguments"
INFILFS_CHECK="$mock_check" INFILFS_SCRUB="$mock_scrub" \
INFILFS_TEST_CHECK_ARGUMENTS="$check_arguments" \
INFILFS_TEST_SCRUB_ARGUMENTS="$scrub_arguments" \
INFILFS_TEST_CHECK_STATUS=0 INFILFS_TEST_SCRUB_STATUS=99 \
    "$fsck_helper" -afnp image
mapfile -t observed < "$check_arguments"
[[ "${observed[*]}" == "--check image" ]]
[[ ! -e "$scrub_arguments" ]]

# Deep scrub is explicit and must not be selected by normal fsck flags.
rm -f "$check_arguments" "$scrub_arguments"
INFILFS_CHECK="$mock_check" INFILFS_SCRUB="$mock_scrub" \
INFILFS_TEST_CHECK_ARGUMENTS="$check_arguments" \
INFILFS_TEST_SCRUB_ARGUMENTS="$scrub_arguments" \
INFILFS_TEST_CHECK_STATUS=99 INFILFS_TEST_SCRUB_STATUS=0 \
    "$fsck_helper" --scrub -n image
mapfile -t observed < "$scrub_arguments"
[[ "${observed[*]}" == "image" ]]
[[ ! -e "$check_arguments" ]]

set +e
INFILFS_CHECK="$mock_check" INFILFS_TEST_CHECK_ARGUMENTS="$check_arguments" \
INFILFS_TEST_CHECK_STATUS=2 "$fsck_helper" image
corrupt_status=$?
INFILFS_CHECK="$mock_check" INFILFS_TEST_CHECK_ARGUMENTS="$check_arguments" \
INFILFS_TEST_CHECK_STATUS=1 "$fsck_helper" image
operational_status=$?
INFILFS_SCRUB="$mock_scrub" INFILFS_TEST_SCRUB_ARGUMENTS="$scrub_arguments" \
INFILFS_TEST_SCRUB_STATUS=2 "$fsck_helper" --scrub image
scrub_corrupt_status=$?
"$fsck_helper" -y image >/dev/null 2>&1
usage_status=$?
set -e
[[ "$corrupt_status" == 4 ]]
[[ "$operational_status" == 8 ]]
[[ "$scrub_corrupt_status" == 4 ]]
[[ "$usage_status" == 16 ]]

echo 'system-utilities: PASS'
