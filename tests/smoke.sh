#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

build_dir="${1:-build}"
mkfs="$build_dir/mkfs.infilfs"
inspect="$build_dir/infilfs-inspect"
tool="$build_dir/infilfs-tool"
fsck="$build_dir/fsck.infiltratorfs"
api_test="$build_dir/infilfs-phase1-api"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
expected_version="$(bash "$repo_root/tools/project-version.sh" "$repo_root/CMakeLists.txt")"
test -n "$expected_version"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
image="$tmp/infilfs.img"
source_file="$tmp/source.bin"
out_file="$tmp/out.bin"
corrupt="$tmp/corrupt.img"
mkfs_output="$tmp/mkfs-output.txt"


# Reject malformed requests before touching even a nonexistent volume.
# Writable open is allowed to heal checkpoints, so dispatch must be fail-closed.
assert_usage_before_open() {
    local rc=0
    "$tool" "$tmp/nonexistent.img" "$@" >"$tmp/invalid.stdout" \
        2>"$tmp/invalid.stderr" || rc=$?
    if [[ "$rc" != 2 ]] ||
       ! grep -Fq 'Usage:' "$tmp/invalid.stderr" ||
       grep -Fq 'open InfiltratorFS' "$tmp/invalid.stderr"; then
        echo "Invalid CLI request attempted a volume open or returned the wrong status: $*" >&2
        cat "$tmp/invalid.stderr" >&2
        exit 1
    fi
}
assert_usage_before_open unsupported-command
assert_usage_before_open mkdir
assert_usage_before_open ls / too-many-arguments
assert_usage_before_open snapshot-restore old /src /dst --typo

truncate -s 64M "$image"
"$mkfs" -L SmokeTest "$image" >"$mkfs_output"
grep -Fq "Implementation: $expected_version" "$mkfs_output"
"$inspect" "$image" >/dev/null
# Normal fsck is the fast structural check. Deep payload verification is only
# selected explicitly with --scrub on the same native fsck executable.
"$fsck" -n "$image" >/dev/null
"$fsck" --scrub -n "$image" >/dev/null
"$api_test" "$image" >/dev/null

# Reformat and exercise the command-line persistence path independently.
"$mkfs" -L SmokeTest "$image" >/dev/null
"$tool" "$image" mkdir /docs
head -c 98765 /dev/urandom > "$source_file"
"$tool" "$image" put "$source_file" /docs/test.bin
"$tool" "$image" cat /docs/test.bin > "$out_file"
cmp "$source_file" "$out_file"

# write must stream source data larger than the fixed I/O buffer, retain the
# prefix before a nonzero offset, and extend the destination exactly.
head -c 786433 /dev/urandom > "$tmp/patch.bin"
"$tool" "$image" write "$tmp/patch.bin" /docs/test.bin 8192
head -c 8192 "$source_file" > "$tmp/expected-patched.bin"
cat "$tmp/patch.bin" >> "$tmp/expected-patched.bin"
"$tool" "$image" cat /docs/test.bin > "$out_file"
cmp "$tmp/expected-patched.bin" "$out_file"
: > "$tmp/empty.bin"
"$tool" "$image" write "$tmp/empty.bin" /docs/test.bin 0
"$tool" "$image" cat /docs/test.bin > "$out_file"
cmp "$tmp/expected-patched.bin" "$out_file"
# Restore the original payload for the existing namespace/reflink regressions.
"$tool" "$image" put "$source_file" /docs/test.bin

# Format 0.19 defines the portable component-name boundary as 1023 UTF-8
# bytes. Exercise the production path parser and directory-tree writer.
name1023="$(head -c 1023 </dev/zero | tr '\0' n)"
name1024="${name1023}n"
"$tool" "$image" put "$source_file" "/docs/$name1023"
"$tool" "$image" cat "/docs/$name1023" > "$out_file"
cmp "$source_file" "$out_file"
if "$tool" "$image" put "$source_file" "/docs/$name1024" >/dev/null 2>&1; then
    echo "1024-byte filename unexpectedly succeeded" >&2
    exit 1
fi
"$tool" "$image" rm "/docs/$name1023"
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
"$fsck" -n "$image" >/dev/null

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
set +e
"$fsck" -n "$corrupt" >/dev/null 2>&1
fsck_corrupt_status=$?
set -e
[[ "$fsck_corrupt_status" == 4 ]]

echo "InfiltratorFS smoke test: PASS"
