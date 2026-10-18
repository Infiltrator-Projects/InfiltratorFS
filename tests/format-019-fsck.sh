#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Qualify experimental Format 0.19 read-only fsck on disposable image files.
set -Eeuo pipefail
[[ $# == 1 ]] || { echo 'Usage: format-019-fsck.sh <build-directory>' >&2; exit 2; }
build="$(realpath "$1")"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
image="$work/format-019.img"
truncate -s 16777216 "$image"
"$build/mkfs.infilfs" --experimental-019-image --force -L Fixture19 "$image" >"$work/mkfs"
grep -Fq 'Format 0.19 image created' "$work/mkfs"
[[ "$(stat -c %s "$image")" == 16777216 ]]
"$build/fsck.infiltratorfs" --scrub -n --experimental-019-image "$image" >"$work/out"
grep -Fq '0.19 (experimental read-only image)' "$work/out"
grep -Fq 'Result:              CLEAN' "$work/out"
# Fast structural 0.19 checking uses the production index/allocation/
# namespace checker but validates checkpoint replicas as Format 0.19.
before="$(sha256sum "$image" | cut -d' ' -f1)"
"$build/fsck.infiltratorfs" -n --experimental-019-image "$image" >"$work/struct"
grep -Fq 'Format:                0.19 (experimental read-only image)' "$work/struct"
grep -Fq 'Checkpoint replicas:   3/3 OK' "$work/struct"
grep -Fq 'Result:                CLEAN' "$work/struct"
[[ "$(sha256sum "$image" | cut -d' ' -f1)" == "$before" ]] || {
    echo 'Unsafe: structural 0.19 fsck modified the image' >&2; exit 1
}
"$build/infilfs-format-019-posix" "$image"
"$build/fsck.infiltratorfs" --scrub -n --experimental-019-image "$image" >"$work/written"
grep -Fq 'Result:              CLEAN' "$work/written"
"$build/fsck.infiltratorfs" -n --experimental-019-image "$image" >"$work/postwrite"
grep -Fq 'Checkpoint replicas:   3/3 OK' "$work/postwrite"
grep -Fq 'Result:                CLEAN' "$work/postwrite"
# Damage one secondary checkpoint; fast check must expose degraded
# redundancy without healing it or modifying the disposable image.
dd if="$image" of="$work/secondary.saved" bs=1 skip=16773120 count=1 status=none
printf '\xff' | dd of="$image" bs=1 seek=16773120 count=1 conv=notrunc status=none
before="$(sha256sum "$image" | cut -d' ' -f1)"
set +e
"$build/fsck.infiltratorfs" -n --experimental-019-image "$image" >"$work/degraded"
result=$?
set -e
[[ "$result" == 4 ]] || {
    echo "Expected degraded 0.19 structural fsck exit 4, got $result" >&2; exit 1
}
grep -Fq 'Checkpoint replicas:   2/3 DEGRADED' "$work/degraded"
grep -Fq 'Result:                FILESYSTEM ERRORS DETECTED' "$work/degraded"
[[ "$(sha256sum "$image" | cut -d' ' -f1)" == "$before" ]] || {
    echo 'Unsafe: degraded 0.19 fsck modified the image' >&2; exit 1
}
dd if="$work/secondary.saved" of="$image" bs=1 seek=16773120 count=1 conv=notrunc status=none
"$build/fsck.infiltratorfs" -n --experimental-019-image "$image" >"$work/restored"
grep -Fq 'Result:                CLEAN' "$work/restored"
if "$build/mkfs.infilfs" --experimental-019-image "$image" >/dev/null 2>&1; then
    echo 'Unsafe: 0.19 image formatting without explicit --force' >&2; exit 1
fi
if "$build/fsck.infiltratorfs" --scrub -a --experimental-019-image "$image" >/dev/null 2>&1; then
    echo 'Unsafe: experimental 0.19 fsck accepted repair' >&2; exit 1
fi
if "$build/fsck.infiltratorfs" -a --experimental-019-image "$image" >/dev/null 2>&1; then
    echo 'Unsafe: 0.19 structural fsck accepted repair' >&2; exit 1
fi
if "$build/fsck.infiltratorfs" --experimental-019-image "$image" >/dev/null 2>&1; then
    echo 'Unsafe: 0.19 structural fsck accepted write-capable default' >&2; exit 1
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
