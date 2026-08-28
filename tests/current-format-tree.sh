#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

build_dir="${1:-build}"
mkfs="$build_dir/mkfs.infilfs"
tool="$build_dir/infilfs-tool"
scrub="$build_dir/infilfs-scrub"
forensic="$build_dir/infilfs-forensic"

for program in "$mkfs" "$tool" "$scrub" "$forensic"; do
    [[ -x "$program" ]] || {
        echo "current-format-tree: missing executable: $program" >&2
        exit 2
    }
done

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
image="$tmp/current-format.img"
empty="$tmp/empty"
records="$tmp/records.tsv"

truncate -s 64M "$image"
truncate -s 0 "$empty"
"$mkfs" -L CurrentFormatTree "$image" >/dev/null
"$forensic" "$image" > "$records"
awk -F '\t' '$2 == "current" && $3 == "object" &&
    $5 == "index" && $6 == 3 { found = 1 } END { exit !found }' "$records"
awk -F '\t' '$2 == "current" && $3 == "object" &&
    $5 == "directory" && $6 == 3 { found = 1 } END { exit !found }' "$records"

for index in $(seq -w 0 219); do
    "$tool" "$image" put "$empty" \
        "/document-${index}-long-name-for-current-directory-tree.txt" \
        >/dev/null
done
[[ "$("$tool" "$image" ls / | wc -l)" -eq 220 ]]
"$scrub" "$image" | grep -Fq 'Result:              CLEAN'
"$forensic" "$image" > "$records"
grep -Fq $'current\tdirectory-branch-page' "$records"
grep -Fq $'current\tindex-branch-page' "$records"

echo 'current-format-tree: PASS'
