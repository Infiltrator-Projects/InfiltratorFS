#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

repo_root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
manager_source="${repo_root}/tools/infiltratorfs-manager.c"
manager_bin="${2:-${repo_root}/build/infiltratorfs-manager}"

test -f "${manager_source}"
test -x "${manager_bin}"

grep -Fq '"lsblk", "-P", "-b", "-n", "-p", "-o"' "${manager_source}"
grep -Fq '"PATH,SIZE,TYPE,RM,PKNAME,TRAN,FSTYPE,LABEL,MOUNTPOINTS"' "${manager_source}"
grep -Fq 'strcmp(type, "part") != 0' "${manager_source}"
grep -Fq 'return "Fixed disk";' "${manager_source}"
grep -Fq -- '--list-partitions' "${manager_source}"
grep -Fq -- '--format-device' "${manager_source}"
grep -Fq 'canonical_path' "${manager_source}"
grep -Fq 'it is not an available non-system partition' "${manager_source}"
grep -Fq 'gtk_application_window_new' "${manager_source}"
grep -Fq 'gtk_stack_switcher_new' "${manager_source}"
grep -Fq 'g_thread_new' "${manager_source}"

if grep -Fqi 'zenity' "${manager_source}"; then
    echo "manager-partition-discovery: Zenity UI regression detected" >&2
    exit 1
fi
if grep -Eq 'python3|PyGObject|gi\.repository' "${manager_source}"; then
    echo "manager-partition-discovery: Python runtime regression detected" >&2
    exit 1
fi

"${manager_bin}" --list-partitions >/dev/null

helper="${repo_root}/tools/infiltratorfs-manager-helper"
grep -Fq 'validate_partition()' "${helper}"
grep -Fq 'forensic-block)' "${helper}"
if grep -Fq 'only a removable, USB, or SD-card partition may be selected' "${helper}"; then
    echo "manager-partition-discovery: fixed-partition rejection remains" >&2
    exit 1
fi

echo "manager-partition-discovery: PASS"
