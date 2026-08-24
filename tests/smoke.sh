#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

build_dir="${1:-build}"
mkfs="$build_dir/mkfs.infilfs"
inspect="$build_dir/infilfs-inspect"
tool="$build_dir/infilfs-tool"
api_test="$build_dir/infilfs-phase1-api"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
image="$tmp/infilfs.img"
source_file="$tmp/source.bin"
out_file="$tmp/out.bin"
corrupt="$tmp/corrupt.img"
mkfs_output="$tmp/mkfs-output.txt"

truncate -s 64M "$image"
"$mkfs" -L SmokeTest "$image" >"$mkfs_output"
grep -Fq 'Implementation: 0.12.0' "$mkfs_output"
"$inspect" "$image" >/dev/null
"$api_test" "$image" >/dev/null

# Reformat and exercise the command-line persistence path independently.
"$mkfs" -L SmokeTest "$image" >/dev/null
"$tool" "$image" mkdir /docs
head -c 98765 /dev/urandom > "$source_file"
"$tool" "$image" put "$source_file" /docs/test.bin
"$tool" "$image" cat /docs/test.bin > "$out_file"
cmp "$source_file" "$out_file"
"$tool" "$image" reflink /docs/test.bin /docs/test-clone.bin
"$tool" "$image" cat /docs/test-clone.bin > "$out_file"
cmp "$source_file" "$out_file"
"$tool" "$image" rm /docs/test-clone.bin
"$tool" "$image" mv /docs/test.bin /docs/renamed.bin
"$tool" "$image" mkdir /archive
"$tool" "$image" mv /docs/renamed.bin /archive/moved.bin
"$tool" "$image" cat /archive/moved.bin > "$out_file"
cmp "$source_file" "$out_file"
"$tool" "$image" rm /archive/moved.bin
"$tool" "$image" rmdir /archive
"$tool" "$image" rmdir /docs
"$inspect" "$image" >/dev/null

# Corrupt the currently referenced root metadata block and require a hard
# rejection. The root is copy-on-write and is not required to remain at the
# formatter's original physical block after mutations.
cp "$image" "$corrupt"
root_block=$("$inspect" "$corrupt" | awk '/Root object:/ {print $4; exit}')
if [[ ! "$root_block" =~ ^[0-9]+$ ]]; then
    echo "corruption test failed: could not determine current root block" >&2
    exit 1
fi
printf '\001' | dd of="$corrupt" bs=1 \
    seek=$((root_block * 4096 + 256)) conv=notrunc status=none
if "$tool" "$corrupt" ls / >/dev/null 2>&1; then
    echo "corruption test failed: damaged root was accepted" >&2
    exit 1
fi

echo "InfiltratorFS smoke test: PASS"
