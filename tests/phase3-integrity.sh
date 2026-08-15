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
        echo "phase3-integrity: expected failpoint exit 97 at $stage, got $rc" >&2
        exit 1
    fi
}

head -c "$((INFS_BLOCK_SIZE=4096))" /dev/zero | tr '\000' 'A' > "$tmp/old.bin"
head -c 4096 /dev/zero | tr '\000' 'B' > "$tmp/new.bin"

base="$tmp/base.img"
truncate -s 64M "$base"
"$mkfs" -L IntegrityTest "$base" >/dev/null
"$tool" "$base" put "$tmp/old.bin" /data
"$scrub" "$base" | grep -Fq 'Result:              CLEAN'
old_physical="$($tool "$base" map /data 0)"

# Before checkpoint publication, an overwrite must leave the old data reachable.
for stage in before-bitmap after-bitmap; do
    image="$tmp/$stage.img"
    cp "$base" "$image"
    expect_crash "$stage" "$tool" "$image" write "$tmp/new.bin" /data 0
    "$tool" "$image" cat /data > "$tmp/readback"
    cmp "$tmp/old.bin" "$tmp/readback"
    "$scrub" "$image" | grep -Fq 'Result:              CLEAN'
done

# The first durable checkpoint is the data commit point too.
committed="$tmp/committed.img"
cp "$base" "$committed"
expect_crash after-checkpoint "$tool" "$committed" write "$tmp/new.bin" /data 0
"$tool" "$committed" cat /data > "$tmp/readback"
cmp "$tmp/new.bin" "$tmp/readback"
new_physical="$($tool "$committed" map /data 0)"
if [[ "$old_physical" == "$new_physical" ]]; then
    echo 'phase3-integrity: committed overwrite reused old physical data block' >&2
    exit 1
fi
"$scrub" "$committed" | grep -Fq 'Result:              CLEAN'


# A partial overwrite must preserve surrounding bytes while still replacing the
# whole physical block and checksum atomically.
head -c 137 /dev/zero | tr '\000' 'C' > "$tmp/patch.bin"
cp "$tmp/old.bin" "$tmp/expected-partial.bin"
dd if="$tmp/patch.bin" of="$tmp/expected-partial.bin" bs=1 seek=1000 conv=notrunc status=none
partial="$tmp/partial.img"
cp "$base" "$partial"
expect_crash after-checkpoint "$tool" "$partial" write "$tmp/patch.bin" /data 1000
"$tool" "$partial" cat /data > "$tmp/partial-readback"
cmp "$tmp/expected-partial.bin" "$tmp/partial-readback"
"$scrub" "$partial" | grep -Fq 'Result:              CLEAN'

# A writable transaction heals the mixed checkpoint set left by after-checkpoint.
"$tool" "$committed" mkdir /heal
inspect_out="$($inspect "$committed")"
generation="$(awk '/Generation:/ {print $2}' <<<"$inspect_out")"
grep -Fq "Checkpoint generations: $generation $generation $generation" <<<"$inspect_out"

# Repeated full-block overwrites must not leak old data or checksum metadata.
free_before="$($inspect "$committed" | awk '/Free blocks:/ {print $3}')"
for _ in $(seq 1 20); do
    "$tool" "$committed" write "$tmp/new.bin" /data 0 >/dev/null
done
free_after="$($inspect "$committed" | awk '/Free blocks:/ {print $3}')"
if [[ "$free_before" != "$free_after" ]]; then
    echo "phase3-integrity: data CoW leaked blocks ($free_before -> $free_after)" >&2
    exit 1
fi

# Cross multiple hidden checksum objects and verify every block.
large="$tmp/large.img"
truncate -s 128M "$large"
"$mkfs" -L ChecksumChain "$large" >/dev/null
dd if=/dev/urandom of="$tmp/large.bin" bs=1M count=3 status=none
"$tool" "$large" put "$tmp/large.bin" /large
"$tool" "$large" cat /large > "$tmp/large-out.bin"
cmp "$tmp/large.bin" "$tmp/large-out.bin"
scrub_out="$($scrub "$large")"
grep -Fq 'Data blocks checked: 768' <<<"$scrub_out"
grep -Fq 'Result:              CLEAN' <<<"$scrub_out"

# Deliberately corrupt one data byte. The scrubber and normal read path must
# both detect the silent corruption from the independently stored checksum.
corrupt="$tmp/corrupt.img"
cp "$base" "$corrupt"
physical="$($tool "$corrupt" map /data 0)"
printf '\377' | dd of="$corrupt" bs=1 seek=$((physical * 4096 + 123)) conv=notrunc status=none
set +e
"$scrub" "$corrupt" > "$tmp/scrub-corrupt.out" 2>&1
scrub_rc=$?
"$tool" "$corrupt" cat /data > /dev/null 2>&1
read_rc=$?
set -e
if [[ $scrub_rc -ne 2 || $read_rc -eq 0 ]]; then
    echo "phase3-integrity: corruption detection failed (scrub=$scrub_rc read=$read_rc)" >&2
    exit 1
fi
grep -Fq 'Checksum errors:     1' "$tmp/scrub-corrupt.out"
grep -Fq 'Result:              CORRUPTION DETECTED' "$tmp/scrub-corrupt.out"

echo 'phase3-integrity: PASS'
