#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
manager="${repo_root}/tools/infiltratorfs-manager"

# The GTK rewrite must preserve full-tree partition discovery. The historic
# regression came from lsblk -d hiding child partitions such as mmcblk0p1.
grep -Fq '"lsblk", "-J", "-b", "-o"' "${manager}"
grep -Fq '"NAME,PATH,SIZE,TYPE,RM,PKNAME,TRAN,FSTYPE,LABEL,MOUNTPOINTS"' "${manager}"
grep -Fq 'if node.get("type") != "part":' "${manager}"
grep -Fq 'return "Fixed disk"' "${manager}"
grep -Fq -- '--list-partitions' "${manager}"
grep -Fq 'Gtk.ApplicationWindow' "${manager}"
grep -Fq 'Gtk.StackSwitcher' "${manager}"
grep -Fq 'threading.Thread' "${manager}"

if grep -Fqi 'zenity' "${manager}"; then
    echo "manager-partition-discovery: Zenity UI regression detected" >&2
    exit 1
fi

# This path deliberately exits before importing GTK, so storage discovery stays
# testable on headless CI runners.
"${manager}" --list-partitions >/dev/null

helper="${repo_root}/tools/infiltratorfs-manager-helper"
grep -Fq 'validate_partition()' "${helper}"
grep -Fq 'forensic-block)' "${helper}"
if grep -Fq 'only a removable, USB, or SD-card partition may be selected' "${helper}"; then
    echo "manager-partition-discovery: fixed-partition rejection remains" >&2
    exit 1
fi

echo "manager-partition-discovery: PASS"
