#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mount_helper="$repo_root/tools/mount.infiltratorfs"
fsck_source="$repo_root/tools/fsck.infiltratorfs.c"
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

# fsck is deliberately a native executable now, not a shell dispatcher. Guard
# the command contract here; functional fast-check/deep-scrub behavior is
# exercised against the built executable by the smoke test.
grep -Fq 'infs_check(&vol, &report)' "$fsck_source"
grep -Fq 'strcmp(arg, "--scrub")' "$fsck_source"
grep -Fq 'infs_scrub_with_progress(&vol, &report, scrub_progress, NULL)' "$fsck_source"
grep -Fq 'infs_scrub_online(&vol, &report)' "$fsck_source"
grep -Fq 'infs_snapshot_scrub(&vol, snapshot_name, &report)' "$fsck_source"
grep -Fq 'FSCK_EXIT_UNCORRECTED 4' "$fsck_source"
grep -Fq 'FSCK_EXIT_OPERATIONAL 8' "$fsck_source"
grep -Fq 'FSCK_EXIT_USAGE 16' "$fsck_source"
! grep -Fq '/usr/bin/infilfs-scrub' "$fsck_source"

echo 'system-utilities: PASS'
