#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

build_dir="${1:-build}"
mkfs="$build_dir/mkfs.infilfs"
inspect="$build_dir/infilfs-inspect"
tool="$build_dir/infilfs-tool"
scrub="$build_dir/infilfs-scrub"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

expect_crash() {
    local stage="$1"
    shift
    set +e
    INFS_TEST_CRASH_STAGE="$stage" "$@" >/dev/null 2>&1
    local rc=$?
    set -e
    if [[ $rc -ne 97 ]]; then
        echo "sparse-files: expected failpoint exit 97 at $stage, got $rc" >&2
        exit 1
    fi
}

assert_stat() {
    local image="$1"
    local expected_size="$2"
    local expected_allocated="$3"
    local output
    output="$($tool "$image" stat /sparse)"
    grep -Fq "size=$expected_size allocated=$expected_allocated" <<<"$output"
}

base="$tmp/base.img"
empty="$tmp/empty"
payload="$tmp/payload"
high_offset=1099511619661
high_block=268435454
block_start=1099511619584
logical_size=1099511619672

: > "$empty"
printf 'tool-sparse' > "$payload"
truncate -s 64M "$base"
"$mkfs" -L SparseCrash "$base" >/dev/null
"$tool" "$base" put "$empty" /sparse
assert_stat "$base" 0 0

# An uncommitted high-offset write must leave the empty file and bitmap intact.
for stage in before-bitmap after-bitmap; do
    image="$tmp/write-$stage.img"
    cp "$base" "$image"
    expect_crash "$stage" "$tool" "$image" write "$payload" /sparse "$high_offset"
    assert_stat "$image" 0 0
    "$scrub" "$image" | grep -Fq 'Data blocks checked: 0'
done

# Checkpoint publication atomically exposes the huge logical hole, one data
# block and its sparse checksum object.
committed="$tmp/write-committed.img"
cp "$base" "$committed"
expect_crash after-checkpoint "$tool" "$committed" write "$payload" /sparse "$high_offset"
assert_stat "$committed" "$logical_size" 4096
[[ "$($tool "$committed" map /sparse 1)" == hole ]]
[[ "$($tool "$committed" map /sparse "$high_block")" != hole ]]
"$scrub" "$committed" | grep -Fq 'Data blocks checked: 1'

# Hole punching follows the same commit rule.
for stage in before-bitmap after-bitmap; do
    image="$tmp/punch-$stage.img"
    cp "$committed" "$image"
    expect_crash "$stage" "$tool" "$image" punch /sparse "$block_start" 4096
    assert_stat "$image" "$logical_size" 4096
    "$scrub" "$image" | grep -Fq 'Data blocks checked: 1'
done

punched="$tmp/punch-committed.img"
cp "$committed" "$punched"
expect_crash after-checkpoint "$tool" "$punched" punch /sparse "$block_start" 4096
assert_stat "$punched" "$logical_size" 0
[[ "$($tool "$punched" map /sparse "$high_block")" == hole ]]
"$scrub" "$punched" | grep -Fq 'Data blocks checked: 0'

# Repeated allocation and reclamation must not leak data, checksum or CoW
# metadata blocks.
"$tool" "$punched" mkdir /heal
free_before="$($inspect "$punched" | awk '/Free blocks:/ {print $3}')"
for _ in $(seq 1 12); do
    "$tool" "$punched" write "$payload" /sparse "$high_offset"
    "$tool" "$punched" punch /sparse "$block_start" 4096
done
free_after="$($inspect "$punched" | awk '/Free blocks:/ {print $3}')"
if [[ "$free_before" != "$free_after" ]]; then
    echo "sparse-files: sparse allocation leaked blocks ($free_before -> $free_after)" >&2
    exit 1
fi

echo 'sparse-files: PASS'
