#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Only formats an image created by this script. Run on a disposable test host.
set -Eeuo pipefail
[[ $EUID -eq 0 ]] || { echo 'Run this qualification as root.' >&2; exit 2; }
build=$(realpath "${1:?build directory required}")
module=$(realpath "${2:?kernel module required}")
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
work=$(mktemp -d)
mnt="$work/mnt"
image="$work/remount.img"
loop=''
loaded=0
cleanup() {
    local rc=$?
    trap - EXIT
    if mountpoint -q "$mnt"; then
        if ! umount "$mnt"; then
            echo "Unmount failed; preserving $work and $loop" >&2
            exit 1
        fi
    fi
    if [[ -n "$loop" ]]; then losetup -d "$loop" || exit 1; fi
    if [[ "$loaded" = 1 ]]; then rmmod infiltratorfs || exit 1; fi
    rm -rf "$work"
    exit "$rc"
}
trap cleanup EXIT
trap 'echo "Remount qualification failed at line $LINENO" >&2' ERR

# Never unload or replace a module being used by the host.
if grep -qw infiltratorfs /proc/filesystems; then
    echo 'InfiltratorFS already loaded; use a clean test host.' >&2
    exit 2
fi
mkdir "$mnt"
truncate -s 512M "$image"
"$build/mkfs.infilfs" --force -L RemountQualification "$image" >/dev/null
printf 'before-remount\n' > "$work/before"
"$build/infilfs-tool" "$image" put "$work/before" /snapshot-live.txt
"$build/infilfs-tool" "$image" snapshot-create before-remount
modprobe lz4_compress
modprobe lz4_decompress
insmod "$module"
loaded=1
loop=$(losetup --find --show "$image")
mount -i -t infiltratorfs -o ro,compress=off,media=balanced "$loop" "$mnt"
python3 "$script_dir/native-remount-qualification.py" "$mnt"
"$build/infiltratorfs-quota" set "$mnt" user 0 32MiB 0 >/dev/null
umount "$mnt"

# Fresh RO mount with persisted quota and snapshot state, then live promotion.
mount -i -t infiltratorfs -o ro,compress=off,media=balanced "$loop" "$mnt"
python3 "$script_dir/native-remount-qualification.py" "$mnt" quota
umount "$mnt"
"$build/infilfs-tool" "$image" cat /snapshot-live.txt > "$work/after"
printf 'after-remount\n' | cmp - "$work/after"
"$build/infilfs-tool" "$image" snapshot-cat before-remount /snapshot-live.txt > "$work/snapshot"
cmp "$work/before" "$work/snapshot"
"$build/infilfs-tool" "$image" cat /remount-work/durable > "$work/durable"
python3 - "$work/durable" <<'PY'
from pathlib import Path
import sys
assert Path(sys.argv[1]).read_bytes() == bytes(range(256)) * (4 * 1024 * 1024 // 256)
PY
"$build/fsck.infiltratorfs" --scrub "$image" | tee "$work/scrub"
grep -Fq 'Result:              CLEAN' "$work/scrub"
echo 'Native live remount qualification: PASS'
