#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

build_dir="${1:-build}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkfs="$build_dir/mkfs.infilfs"
inspect="$build_dir/infilfs-inspect"
rules="$repo_root/packaging/59-infiltratorfs.rules"
udisks_patch="$repo_root/packaging/udisks2-infiltratorfs-display.patch"
bootstrap="$repo_root/support/installer/bootstrap.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

image="$tmp/desktop.img"
plain="$tmp/plain.img"
probe="$tmp/probe.txt"

truncate -s 64M "$image"
"$mkfs" -L 'Desktop Test' "$image" >/dev/null
"$inspect" --udev "$image" > "$probe"

grep -Fxq 'ID_FS_USAGE=filesystem' "$probe"
grep -Fxq 'ID_FS_TYPE=infiltratorfs' "$probe"
grep -Fxq 'ID_FS_VERSION=0.14' "$probe"
grep -Fxq 'ID_FS_LABEL=Desktop Test' "$probe"
grep -Fxq 'ID_FS_BLOCK_SIZE=4096' "$probe"
grep -Eq '^ID_FS_UUID=[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$' "$probe"

truncate -s 64M "$plain"
if "$inspect" --udev "$plain" > "$tmp/plain-probe.txt" 2>/dev/null; then
    echo 'desktop-integration: non-InfiltratorFS image was identified' >&2
    exit 1
fi
[[ ! -s "$tmp/plain-probe.txt" ]]

grep -Fq 'ID_FS_TYPE}=="infiltratorfs"' "$rules"
grep -Fq 'UDISKS_MOUNT_OPTIONS_DEFAULTS' "$rules"
grep -Fq 'modprobe -q infiltratorfs' "$rules"
grep -Fq 'ENV{ID_FS_LABEL}=="", ENV{UDISKS_NAME}="InfiltratorFS"' "$rules"
grep -Fq 'UDISKS_ICON_NAME}="drive-harddisk"' "$rules"
grep -Fq 'UDISKS_SYMBOLIC_ICON_NAME}="drive-harddisk-symbolic"' "$rules"

# The installer refreshes the block-device udev database before returning.
# Because the udev rule probes InfiltratorFS using a shared advisory lock,
# require the installer to wait for those probes to drain before callers can
# immediately invoke mkfs on the same device.
grep -Fq 'udevadm trigger --subsystem-match=block --action=change' "$bootstrap"
grep -Fq 'udevadm settle --timeout=30' "$bootstrap"

# libudisks2 currently owns the long human-readable filesystem description
# consumed by GNOME Disks.  Keep the minimal upstream mapping in-tree so the
# project cannot regress into pretending that udev metadata alone controls the
# "Contents" string.
test -s "$udisks_patch"
grep -Fq '"filesystem", "infiltratorfs",     "*"' "$udisks_patch"
grep -Fq 'InfiltratorFS (format %s)' "$udisks_patch"
grep -Fq '"filesystem", "infiltratorfs",     NULL' "$udisks_patch"

echo 'desktop-integration: PASS'
