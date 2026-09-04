#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

build_dir="${1:-build}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
volume="$root/src/volume.c"
volume_dir="$root/src/volume"
mkfs="$build_dir/mkfs.infilfs"
tool="$build_dir/infilfs-tool"
scrub="$build_dir/infilfs-scrub"
forensic="$build_dir/infilfs-forensic"

# Portable-core maintainability is a structural invariant.  Historical phase
# and part numbering hid subsystem ownership, and nested implementation
# includes made ordering dependencies invisible.  Keep one explicit compositor
# in src/volume.c and require every implementation unit to be responsibility-
# named and leaf-only.
[[ -f "$volume" ]] || {
    echo "current-format-tree: missing portable volume compositor" >&2
    exit 2
}
if find "$volume_dir" -type f \( -name 'part[0-9]*.inc' -o -path '*/phase[0-9]*/*' \) \
        -print -quit | grep -q .; then
    echo "current-format-tree: historical phase/part implementation unit returned" >&2
    exit 1
fi
if grep -RIlE '#include[[:space:]]+"[^"]+\.inc"' "$volume_dir" \
        --include='*.inc' | grep -q .; then
    echo "current-format-tree: nested portable implementation include detected" >&2
    exit 1
fi
while IFS= read -r compositor; do
    [[ "$compositor" = "$volume" ]] || {
        echo "current-format-tree: unexpected portable implementation compositor: $compositor" >&2
        exit 1
    }
done < <(grep -RIlE '#include[[:space:]]+"volume/[^"]+\.inc"' \
    "$root/src" --include='*.c')
grep -Fq '#include "volume/core.inc"' "$volume"
grep -Fq '#include "volume/checkpoint-recovery.inc"' "$volume"
grep -Fq '#include "volume/checkpoint-publication.inc"' "$volume"
grep -Fq '#include "volume/file-read.inc"' "$volume"
grep -Fq '#include "volume/file-write.inc"' "$volume"
grep -Fq '#include "volume/namespace-replace.inc"' "$volume"

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
