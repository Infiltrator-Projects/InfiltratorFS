#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

build_dir="${1:-build}"
mkfs="$build_dir/mkfs.infilfs"
tool="$build_dir/infilfs-tool"
forensic="$build_dir/infilfs-forensic"

for program in "$mkfs" "$tool" "$forensic"; do
    [[ -x "$program" ]] || {
        echo "forensic-scan: missing executable: $program" >&2
        exit 2
    }
done

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
image="$tmp/forensic.img"
corrupt="$tmp/corrupt.img"
checkpointless="$tmp/checkpointless.img"
source_file="$tmp/source.bin"
records="$tmp/records.tsv"
json="$tmp/records.jsonl"
corrupt_json="$tmp/corrupt.jsonl"

truncate -s 64M "$image"
"$mkfs" -L ForensicScan "$image" >/dev/null
"$tool" "$image" mkdir /evidence
head -c 16384 /dev/urandom > "$source_file"
"$tool" "$image" put "$source_file" /evidence/original.bin
"$tool" "$image" mv /evidence/original.bin /evidence/renamed.bin

"$forensic" "$image" > "$records"
"$forensic" --jsonl "$image" > "$json"
grep -Fq $'current\tcheckpoint' "$records"
grep -Fq $'current\tobject' "$records"
grep -Fq $'orphaned\tobject' "$records"
grep -Fq '"record":"summary"' "$json"
grep -Fq '"allocation_map_available":true' "$json"
grep -Eq '"orphaned_records":[1-9][0-9]*' "$json"

orphan_block="$(awk -F '\t' '$2 == "orphaned" && $3 == "object" {print $1; exit}' "$records")"
[[ "$orphan_block" =~ ^[0-9]+$ ]]
cp "$image" "$corrupt"
printf 'X' | dd of="$corrupt" bs=1 seek="$((orphan_block * 4096))" \
    conv=notrunc status=none
"$forensic" --jsonl "$corrupt" > "$corrupt_json"
before="$(sed -n 's/.*"records_found":\([0-9][0-9]*\).*/\1/p' "$json" | tail -n1)"
after="$(sed -n 's/.*"records_found":\([0-9][0-9]*\).*/\1/p' "$corrupt_json" | tail -n1)"
[[ "$before" =~ ^[0-9]+$ && "$after" =~ ^[0-9]+$ ]]
[[ "$after" -eq $((before - 1)) ]]

cp "$image" "$checkpointless"
for block in 0 8192 16383; do
    dd if=/dev/zero of="$checkpointless" bs=4096 seek="$block" count=1 \
        conv=notrunc status=none
done
"$forensic" --jsonl "$checkpointless" > "$json"
grep -Fq '"allocation_map_available":false' "$json"
grep -Fq '"state":"unknown","kind":"object"' "$json"

echo 'forensic-scan: PASS'
