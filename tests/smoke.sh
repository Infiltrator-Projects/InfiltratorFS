#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

build_dir="${1:-build}"
mkfs="$build_dir/mkfs.infilfs"
inspect="$build_dir/infilfs-inspect"
tool="$build_dir/infilfs-tool"
scrub="$build_dir/infilfs-scrub"
api_test="$build_dir/infilfs-phase1-api"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fsck_helper="$repo_root/tools/fsck.infiltratorfs"
expected_version="$(sed -nE 's/^project\(InfiltratorFS VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES C\)$/\1/p' "$repo_root/CMakeLists.txt")"
test -n "$expected_version"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
image="$tmp/infilfs.img"
source_file="$tmp/source.bin"
out_file="$tmp/out.bin"
corrupt="$tmp/corrupt.img"
mkfs_output="$tmp/mkfs-output.txt"

truncate -s 64M "$image"
"$mkfs" -L SmokeTest "$image" >"$mkfs_output"
grep -Fq "Implementation: $expected_version" "$mkfs_output"
"$inspect" "$image" >/dev/null
INFILFS_SCRUB="$scrub" "$fsck_helper" -n "$image" >/dev/null
"$api_test" "$image" >/dev/null

# Reformat and exercise the command-line persistence path independently.
"$mkfs" -L SmokeTest "$image" >/dev/null
"$tool" "$image" mkdir /docs
head -c 98765 /dev/urandom > "$source_file"
"$tool" "$image" put "$source_file" /docs/test.bin
"$tool" "$image" cat /docs/test.bin > "$out_file"
cmp "$source_file" "$out_file"

# Format 0.15 doubles the portable component-name boundary from 255 to 510
# UTF-8 bytes. Exercise the production path parser and directory-tree writer.
name510="$(head -c 510 </dev/zero | tr '\0' n)"
name511="${name510}n"
"$tool" "$image" put "$source_file" "/docs/$name510"
"$tool" "$image" cat "/docs/$name510" > "$out_file"
cmp "$source_file" "$out_file"
if "$tool" "$image" put "$source_file" "/docs/$name511" >/dev/null 2>&1; then
    echo "511-byte filename unexpectedly succeeded" >&2
    exit 1
fi
"$tool" "$image" rm "/docs/$name510"
"$tool" "$image" reflink /docs/test.bin /docs/test-clone.bin
"$tool" "$image" cat /docs/test-clone.bin > "$out_file"
cmp "$source_file" "$out_file"
"$tool" "$image" rm /docs/test-clone.bin
"$tool" "$image" mv /docs/test.bin /docs/renamed.bin
"$tool" "$image" mkdir /archive
"$tool" "$image" mv /docs/renamed.bin /archive/moved.bin
"$tool" "$image" cat /archive/moved.bin > "$out_file"
cmp "$source_file" "$out_file"
"$tool" "$image" symlink ../archive/moved.bin /docs/current
[[ "$("$tool" "$image" readlink /docs/current)" == ../archive/moved.bin ]]
"$tool" "$image" ls /docs | grep -Fq 'l current'
"$tool" "$image" mv /docs/current /archive/current
[[ "$("$tool" "$image" readlink /archive/current)" == ../archive/moved.bin ]]
"$tool" "$image" rm /archive/current
"$tool" "$image" link /archive/moved.bin /docs/hard-link.bin
"$tool" "$image" cat /docs/hard-link.bin > "$out_file"
cmp "$source_file" "$out_file"
[[ "$("$tool" "$image" stat /archive/moved.bin)" == *"links=2"* ]]
"$tool" "$image" snapshot-create before-removal
"$tool" "$image" snapshot-list | grep -Eq '^before-removal generation='
"$tool" "$image" rm /archive/moved.bin
"$tool" "$image" cat /docs/hard-link.bin > "$out_file"
cmp "$source_file" "$out_file"
"$tool" "$image" snapshot-cat before-removal /archive/moved.bin > "$out_file"
cmp "$source_file" "$out_file"
"$tool" "$image" rm /docs/hard-link.bin
"$tool" "$image" rmdir /archive
"$tool" "$image" rmdir /docs
"$tool" "$image" snapshot-delete before-removal
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
