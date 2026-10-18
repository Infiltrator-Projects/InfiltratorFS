#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Qualify experimental Format 0.19 read-only fsck on disposable image files.
set -Eeuo pipefail
[[ $# == 1 ]] || { echo 'Usage: format-019-fsck.sh <build-directory>' >&2; exit 2; }
build="$(realpath "$1")"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
image="$work/format-019.img"
"$build/infilfs-format-019-image" --emit-image "$image" >/dev/null
[[ "$(stat -c %s "$image")" == 16777216 ]]
"$build/fsck.infiltratorfs" --scrub -n --experimental-019-image "$image" >"$work/out"
grep -Fq '0.19 (experimental read-only image)' "$work/out"
grep -Fq 'Result:              CLEAN' "$work/out"
if "$build/fsck.infiltratorfs" --scrub -a --experimental-019-image "$image" >/dev/null 2>&1; then
    echo 'Unsafe: experimental 0.19 fsck accepted repair' >&2; exit 1
fi
# Destroy both locator pages; the valid primary may still provide their
# independently checked backup coordinates.
printf '\xff' | dd of="$image" bs=1 seek=4096 count=1 conv=notrunc status=none
printf '\xff' | dd of="$image" bs=1 seek=8384512 count=1 conv=notrunc status=none
"$build/fsck.infiltratorfs" --scrub -n --experimental-019-image "$image" >"$work/recovered"
grep -Fq 'Result:              CLEAN' "$work/recovered"
# No locator nor valid block-zero checkpoint: no speculative geometry scan.
printf '\xff' | dd of="$image" bs=1 seek=0 count=1 conv=notrunc status=none
set +e
"$build/fsck.infiltratorfs" --scrub -n --experimental-019-image "$image" >/dev/null 2>&1
result=$?
set -e
[[ "$result" == 4 ]] || {
    echo "Unsafe: unrecoverable experimental 0.19 image exited $result, expected 4" >&2
    exit 1
}
echo 'Format 0.19 read-only image fsck: PASS'
