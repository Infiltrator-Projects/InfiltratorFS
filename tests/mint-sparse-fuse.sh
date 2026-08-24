#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail

build_dir="${1:-build}"
mkfs="$build_dir/mkfs.infilfs"
tool="$build_dir/infilfs-tool"
scrub="$build_dir/infilfs-scrub"
fuse="$build_dir/infilfs-fuse"
handle_test="$build_dir/infilfs-fuse-open-handles"

if [[ ! -r /dev/fuse || ! -w /dev/fuse ]] ||
   ! command -v fusermount3 >/dev/null 2>&1; then
    echo 'mint-sparse-fuse: SKIP (/dev/fuse access or fusermount3 unavailable)'
    exit 77
fi

for program in "$mkfs" "$tool" "$scrub" "$fuse" "$handle_test"; do
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
fuse_log="$tmp/fuse.log"
fuse_pid=""
holder_pid=""
current_step="initialization"

report_error() {
    local line="$1"
    local command="$2"
    local status="$3"
    set +e
    echo "mint-sparse-fuse: FAIL during $current_step" >&2
    echo "mint-sparse-fuse: line $line exited $status: $command" >&2
    if [[ -s "$fuse_log" ]]; then
        echo 'mint-sparse-fuse: FUSE log follows:' >&2
        sed 's/^/  /' "$fuse_log" >&2
    fi
}

cleanup() {
    if mountpoint -q "$mount_dir" 2>/dev/null; then
        fusermount3 -u "$mount_dir" 2>/dev/null || true
    fi
    if [[ -n "$fuse_pid" ]]; then
        wait "$fuse_pid" 2>/dev/null || true
    fi
    if [[ -n "$holder_pid" ]]; then
        kill -TERM "$holder_pid" 2>/dev/null || true
        wait "$holder_pid" 2>/dev/null || true
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT
trap 'report_error "$LINENO" "$BASH_COMMAND" "$?"' ERR

step() {
    current_step="$1"
    echo "mint-sparse-fuse: $current_step"
}

mount_image() {
    : > "$fuse_log"
    "$fuse" "$image" "$mount_dir" -f >"$fuse_log" 2>&1 &
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

interrupt_with_open_unlink() {
    local ready="$tmp/interrupted-ready"
    "$handle_test" --hold-unlinked "$mount_dir" "$ready" &
    holder_pid=$!
    for _ in $(seq 1 50); do
        [[ -f "$ready" ]] && break
        kill -0 "$holder_pid" 2>/dev/null
        sleep 0.1
    done
    [[ -f "$ready" ]]
    kill -KILL "$fuse_pid"
    wait "$fuse_pid" 2>/dev/null || true
    fuse_pid=""
    if mountpoint -q "$mount_dir" 2>/dev/null; then
        fusermount3 -uz "$mount_dir"
    fi
    kill -TERM "$holder_pid" 2>/dev/null || true
    wait "$holder_pid" 2>/dev/null || true
    holder_pid=""
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

step 'mounting the freshly formatted image'
mount_image
step 'verifying rename and unlink lifetime for open file descriptors'
"$handle_test" "$mount_dir"
step 'interrupting a mount with an unlinked descriptor retained'
interrupt_with_open_unlink
step 'remounting to reclaim the interrupted open handle'
mount_image
unmount_image
if "$tool" "$image" ls / | grep -Fq '.infilfs-open-handles-'; then
    echo 'mint-sparse-fuse: stale open-handle directory survived remount' >&2
    exit 1
fi
mount_image
step 'creating and mutating an ordinary nested file'
mkdir -p "$mount_dir/tree/subdirectory"
cp "$ordinary_source" "$mount_dir/tree/subdirectory/original.bin"
chmod 0640 "$mount_dir/tree/subdirectory/original.bin"
mv "$mount_dir/tree/subdirectory/original.bin" \
    "$mount_dir/tree/subdirectory/renamed.bin"
cmp "$ordinary_source" "$mount_dir/tree/subdirectory/renamed.bin"
[[ "$(stat -c '%a' "$mount_dir/tree/subdirectory/renamed.bin")" == 640 ]]
step 'creating and resolving native symbolic links'
ln -s renamed.bin "$mount_dir/tree/subdirectory/relative-link"
ln -s /tree/subdirectory/renamed.bin "$mount_dir/absolute-link"
[[ "$(readlink "$mount_dir/tree/subdirectory/relative-link")" == renamed.bin ]]
[[ "$(readlink "$mount_dir/absolute-link")" == /tree/subdirectory/renamed.bin ]]
cmp "$ordinary_source" "$mount_dir/tree/subdirectory/relative-link"
cmp "$ordinary_source" "$mount_dir/absolute-link"
step 'creating a 1 TiB sparse file'
truncate -s "$logical_size" "$mount_dir/sparse.bin"
read -r size blocks < <(stat -c '%s %b' "$mount_dir/sparse.bin")
[[ "$size" == "$logical_size" && "$blocks" == 0 ]]

step 'writing and reading the high sparse block'
dd if="$source_block" of="$mount_dir/sparse.bin" bs=4096 \
    seek="$logical_block" count=1 conv=notrunc status=none
sync
read -r size blocks < <(stat -c '%s %b' "$mount_dir/sparse.bin")
[[ "$size" == "$logical_size" && "$blocks" == 8 ]]
dd if="$mount_dir/sparse.bin" of="$readback" bs=4096 \
    skip="$logical_block" count=1 status=none
cmp "$source_block" "$readback"

step 'punching and verifying the high sparse block'
fallocate --punch-hole --keep-size --offset "$block_offset" \
    --length 4096 "$mount_dir/sparse.bin"
sync
read -r size blocks < <(stat -c '%s %b' "$mount_dir/sparse.bin")
[[ "$size" == "$logical_size" && "$blocks" == 0 ]]
dd if="$mount_dir/sparse.bin" of="$readback" bs=4096 \
    skip="$logical_block" count=1 status=none
cmp "$zero_block" "$readback"
step 'unmounting before offline verification'
unmount_image

step 'verifying the unmounted image'
[[ "$($tool "$image" map /sparse.bin "$logical_block")" == hole ]]
"$scrub" "$image" | grep -Fq 'Data blocks checked: 0'

step 'remounting and checking persistence'
mount_image
cp "$mount_dir/tree/subdirectory/renamed.bin" "$ordinary_readback"
cmp "$ordinary_source" "$ordinary_readback"
[[ "$(stat -c '%a' "$mount_dir/tree/subdirectory/renamed.bin")" == 640 ]]
[[ "$(readlink "$mount_dir/tree/subdirectory/relative-link")" == renamed.bin ]]
[[ "$(readlink "$mount_dir/absolute-link")" == /tree/subdirectory/renamed.bin ]]
cmp "$ordinary_source" "$mount_dir/tree/subdirectory/relative-link"
read -r size blocks < <(stat -c '%s %b' "$mount_dir/sparse.bin")
[[ "$size" == "$logical_size" && "$blocks" == 0 ]]
dd if="$mount_dir/sparse.bin" of="$readback" bs=4096 \
    skip="$logical_block" count=1 status=none
cmp "$zero_block" "$readback"
step 'performing the final unmount'
unmount_image

echo 'mint-sparse-fuse: PASS'
