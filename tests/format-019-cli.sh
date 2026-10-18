#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail
build="$(realpath "${1:?build directory required}")"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
image="$work/test.img"
for bytes in 16777215 16777217 16781311; do
    truncate -s "$bytes" "$image"
    before="$(sha256sum "$image" | cut -d' ' -f1)"
    if "$build/mkfs.infilfs" --force "$image" >"$work/out" 2>&1; then
        echo "Formatter accepted invalid size $bytes" >&2
        exit 1
    fi
    [[ "$(sha256sum "$image" | cut -d' ' -f1)" == "$before" ]]
done
truncate -s 16M "$image"
"$build/mkfs.infilfs" --force "$image" | grep -Fq 'Format:          0.19'
"$build/fsck.infiltratorfs" -n "$image" | grep -Fq 'Recovery locators:     2/2 OK'
for block in 1 2047; do
    printf '\xff' | dd of="$image" bs=1 seek="$((block * 4096))" count=1 conv=notrunc status=none
done
set +e
"$build/fsck.infiltratorfs" -n "$image" >"$work/degraded"
status=$?
set -e
[[ "$status" == 4 ]]
grep -Fq 'Recovery locators:     0/2 DEGRADED' "$work/degraded"
set +e
"$build/fsck.infiltratorfs" -a "$image" >"$work/repaired"
status=$?
set -e
[[ "$status" == 1 ]]
grep -Fq 'Recovery locators:     2/2 OK' "$work/repaired"
"$build/fsck.infiltratorfs" -n "$image" | grep -Fq 'Result:                CLEAN'
echo 'Format 0.19 default CLI admission and locator repair: PASS'
