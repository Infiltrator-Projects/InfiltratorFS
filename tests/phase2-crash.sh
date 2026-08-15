#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

build_dir="${1:-build}"
mkfs="$build_dir/mkfs.infilfs"
inspect="$build_dir/infilfs-inspect"
tool="$build_dir/infilfs-tool"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
image="$tmp/infilfs.img"

expect_crash() {
    local stage="$1"
    shift
    set +e
    INFS_TEST_CRASH_STAGE="$stage" "$@" >/dev/null 2>&1
    local rc=$?
    set -e
    if [[ $rc -ne 97 ]]; then
        echo "phase2-crash: expected failpoint exit 97 at $stage, got $rc" >&2
        exit 1
    fi
}

contains_name() {
    local name="$1"
    "$tool" "$image" ls / | awk '{print $2}' | grep -Fxq "$name"
}

truncate -s 64M "$image"
"$mkfs" -L CrashTest "$image" >/dev/null

# Before the bitmap is published, no durable pointer reaches the new metadata.
expect_crash before-bitmap "$tool" "$image" mkdir /before_bitmap
"$inspect" "$image" | grep -Fq 'Generation:      1'
if contains_name before_bitmap; then
    echo 'phase2-crash: pre-bitmap transaction became visible' >&2
    exit 1
fi

# A durable but unpublished bitmap is still unreachable from generation 1.
expect_crash after-bitmap "$tool" "$image" mkdir /after_bitmap
"$inspect" "$image" | grep -Fq 'Generation:      1'
if contains_name after_bitmap; then
    echo 'phase2-crash: unpublished bitmap transaction became visible' >&2
    exit 1
fi

# The first durable checkpoint is the atomic commit point.
expect_crash after-checkpoint "$tool" "$image" mkdir /committed
inspect_out="$($inspect "$image")"
grep -Fq 'Generation:      2' <<<"$inspect_out"
grep -Eq 'Checkpoint generations: (2 1 1|1 2 1|1 1 2)' <<<"$inspect_out"
contains_name committed

# A writable open must heal the mixed checkpoint set before allocating again.
"$tool" "$image" mkdir /healed
inspect_out="$($inspect "$image")"
grep -Fq 'Generation:      3' <<<"$inspect_out"
grep -Fq 'Checkpoint generations: 3 3 3' <<<"$inspect_out"
contains_name committed
contains_name healed

# A crash before commit of a removal must retain the old directory entry.
expect_crash after-bitmap "$tool" "$image" rmdir /healed
contains_name healed

# A crash after the removal checkpoint must make the deletion durable.
expect_crash after-checkpoint "$tool" "$image" rmdir /healed
if contains_name healed; then
    echo 'phase2-crash: committed removal was lost' >&2
    exit 1
fi
contains_name committed

# Heal once more and require all three physical checkpoint copies to agree.
"$tool" "$image" mkdir /final_heal
inspect_out="$($inspect "$image")"
generation="$(awk '/Generation:/ {print $2}' <<<"$inspect_out")"
grep -Fq "Checkpoint generations: $generation $generation $generation" <<<"$inspect_out"


# Failed mutations must abort their in-memory transaction without advancing the generation.
before_failed="$($inspect "$image")"
before_failed_gen="$(awk '/Generation:/ {print $2}' <<<"$before_failed")"
before_failed_free="$(awk '/Free blocks:/ {print $3}' <<<"$before_failed")"
if "$tool" "$image" mkdir /committed >/dev/null 2>&1; then
    echo 'phase2-crash: duplicate mkdir unexpectedly succeeded' >&2
    exit 1
fi
after_failed="$($inspect "$image")"
[[ "$(awk '/Generation:/ {print $2}' <<<"$after_failed")" == "$before_failed_gen" ]]
[[ "$(awk '/Free blocks:/ {print $3}' <<<"$after_failed")" == "$before_failed_free" ]]

# Repeated metadata-only CoW commits must reclaim superseded blocks.
"$tool" "$image" mkdir /stable
free_before="$($inspect "$image" | awk '/Free blocks:/ {print $3}')"
for _ in $(seq 1 12); do
    "$tool" "$image" mv /stable /stable2
    "$tool" "$image" mv /stable2 /stable
done
free_after="$($inspect "$image" | awk '/Free blocks:/ {print $3}')"
if [[ "$free_before" != "$free_after" ]]; then
    echo "phase2-crash: CoW metadata leaked blocks ($free_before -> $free_after)" >&2
    exit 1
fi


# A successful metadata transaction must move metadata and bitmap roots rather
# than overwrite their committed physical blocks in place.
cow_image="$tmp/cow.img"
truncate -s 64M "$cow_image"
"$mkfs" -L CowTest "$cow_image" >/dev/null
before="$($inspect "$cow_image")"
before_bitmap="$(sed -n 's/.*Bitmap:          block \([0-9][0-9]*\),.*/\1/p' <<<"$before")"
before_index="$(sed -n 's/.*Object index:    block \([0-9][0-9]*\).*/\1/p' <<<"$before")"
before_root="$(sed -n 's/.*Root object:     block \([0-9][0-9]*\).*/\1/p' <<<"$before")"
"$tool" "$cow_image" mkdir /cow
After="$($inspect "$cow_image")"
after_bitmap="$(sed -n 's/.*Bitmap:          block \([0-9][0-9]*\),.*/\1/p' <<<"$After")"
after_index="$(sed -n 's/.*Object index:    block \([0-9][0-9]*\).*/\1/p' <<<"$After")"
after_root="$(sed -n 's/.*Root object:     block \([0-9][0-9]*\).*/\1/p' <<<"$After")"
[[ "$before_bitmap" != "$after_bitmap" ]]
[[ "$before_index" != "$after_index" ]]
[[ "$before_root" != "$after_root" ]]

echo 'phase2-crash: PASS'
