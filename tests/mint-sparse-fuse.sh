#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

build_dir="${1:-build}"
mkfs="$build_dir/mkfs.infilfs"
tool="$build_dir/infilfs-tool"
scrub="$build_dir/infilfs-scrub"
fuse="$build_dir/infilfs-fuse"

if [[ ! -r /dev/fuse || ! -w /dev/fuse ]] ||
   ! command -v fusermount3 >/dev/null 2>&1; then
    echo 'mint-sparse-fuse: SKIP (/dev/fuse access or fusermount3 unavailable)'
    exit 77
fi

for program in "$mkfs" "$tool" "$scrub" "$fuse"; do
    if [[ ! -x "$program" ]]; then
        echo "mint-sparse-fuse: missing executable: $program" >&2
        exit 2
    fi
done

tmp="$(mktemp -d)"
image="$tmp/sparse.img"
mount_dir="$tmp/mnt"
source_block="$tmp/source.bin"
readback="$tmp/readback.bin"
zero_block="$tmp/zero.bin"
ordinary_source="$tmp/ordinary-source.bin"
ordinary_readback="$tmp/ordinary-readback.bin"
fuse_pid=""

cleanup() {
    if mountpoint -q "$mount_dir" 2>/dev/null; then
        fusermount3 -u "$mount_dir" 2>/dev/null || true
    fi
    if [[ -n "$fuse_pid" ]]; then
        wait "$fuse_pid" 2>/dev/null || true
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT

mount_image() {
    "$fuse" "$image" "$mount_dir" -f &
    fuse_pid=$!
    for _ in $(seq 1 50); do
        if mountpoint -q "$mount_dir"; then
            return 0
        fi
        sleep 0.1
    done
    echo 'mint-sparse-fuse: filesystem did not mount' >&2
    return 1
}

unmount_image() {
    fusermount3 -u "$mount_dir"
    wait "$fuse_pid" 2>/dev/null || true
    fuse_pid=""
}

logical_size=$((1024 * 1024 * 1024 * 1024))
logical_block=268435454
block_offset=$((logical_block * 4096))

mkdir -p "$mount_dir"
truncate -s 256M "$image"
"$mkfs" -L Mint-Sparse-FUSE "$image" >/dev/null
head -c 4096 /dev/zero | tr '\000' 'S' > "$source_block"
head -c 4096 /dev/zero > "$zero_block"
head -c 98765 /dev/urandom > "$ordinary_source"

mount_image
mkdir -p "$mount_dir/tree/subdirectory"
cp "$ordinary_source" "$mount_dir/tree/subdirectory/original.bin"
chmod 0640 "$mount_dir/tree/subdirectory/original.bin"
mv "$mount_dir/tree/subdirectory/original.bin" \
    "$mount_dir/tree/subdirectory/renamed.bin"
cmp "$ordinary_source" "$mount_dir/tree/subdirectory/renamed.bin"
[[ "$(stat -c '%a' "$mount_dir/tree/subdirectory/renamed.bin")" == 640 ]]
truncate -s "$logical_size" "$mount_dir/sparse.bin"
read -r size blocks < <(stat -c '%s %b' "$mount_dir/sparse.bin")
[[ "$size" == "$logical_size" && "$blocks" == 0 ]]

dd if="$source_block" of="$mount_dir/sparse.bin" bs=4096 \
    seek="$logical_block" count=1 conv=notrunc status=none
sync
read -r size blocks < <(stat -c '%s %b' "$mount_dir/sparse.bin")
[[ "$size" == "$logical_size" && "$blocks" == 8 ]]
dd if="$mount_dir/sparse.bin" of="$readback" bs=4096 \
    skip="$logical_block" count=1 status=none
cmp "$source_block" "$readback"

fallocate --punch-hole --keep-size --offset "$block_offset" \
    --length 4096 "$mount_dir/sparse.bin"
sync
read -r size blocks < <(stat -c '%s %b' "$mount_dir/sparse.bin")
[[ "$size" == "$logical_size" && "$blocks" == 0 ]]
dd if="$mount_dir/sparse.bin" of="$readback" bs=4096 \
    skip="$logical_block" count=1 status=none
cmp "$zero_block" "$readback"
unmount_image

[[ "$($tool "$image" map /sparse.bin "$logical_block")" == hole ]]
"$scrub" "$image" | grep -Fq 'Data blocks checked: 0'

mount_image
cp "$mount_dir/tree/subdirectory/renamed.bin" "$ordinary_readback"
cmp "$ordinary_source" "$ordinary_readback"
[[ "$(stat -c '%a' "$mount_dir/tree/subdirectory/renamed.bin")" == 640 ]]
read -r size blocks < <(stat -c '%s %b' "$mount_dir/sparse.bin")
[[ "$size" == "$logical_size" && "$blocks" == 0 ]]
dd if="$mount_dir/sparse.bin" of="$readback" bs=4096 \
    skip="$logical_block" count=1 status=none
cmp "$zero_block" "$readback"
unmount_image

echo 'mint-sparse-fuse: PASS'
