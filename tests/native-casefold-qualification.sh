#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

build="${1:-build}"
module="${2:-kernel/infiltratorfs.ko}"
image="$(mktemp --suffix=.img)"
mountpoint="$(mktemp -d)"
loopdev=""
mounted=0
loaded=0

cleanup() {
    set +e
    if [[ "$mounted" = 1 ]]; then sudo umount "$mountpoint"; fi
    if [[ -n "$loopdev" ]]; then sudo losetup -d "$loopdev"; fi
    if [[ "$loaded" = 1 ]]; then sudo rmmod infiltratorfs; fi
    rm -f "$image"
    rmdir "$mountpoint" 2>/dev/null || true
}
trap cleanup EXIT

truncate -s 128M "$image"
"$build/mkfs.infilfs" --casefold -L casefold "$image"
loopdev="$(sudo losetup --find --show "$image")"

if ! lsmod | grep -q '^infiltratorfs '; then
    sudo insmod "$module"
    loaded=1
fi
sudo mount -t infiltratorfs -o rw "$loopdev" "$mountpoint"
mounted=1

printf 'mixed-case payload\n' | sudo tee "$mountpoint/ReadMe.TXT" >/dev/null
test "$(sudo cat "$mountpoint/readme.txt")" = "mixed-case payload"
test "$(sudo stat -c '%i' "$mountpoint/ReadMe.TXT")" =      "$(sudo stat -c '%i' "$mountpoint/README.TXT")"

sudo python3 - "$mountpoint" <<'PY'
import os, sys
root = sys.argv[1]
try:
    fd = os.open(os.path.join(root, "rEaDmE.TxT"),
                 os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
except FileExistsError:
    pass
else:
    os.close(fd)
    raise SystemExit("case-fold O_EXCL incorrectly created a duplicate name")
PY

# v1 is deliberately ASCII-only: non-ASCII UTF-8 remains byte-exact.
sudo touch "$mountpoint/Ä.txt"
sudo touch "$mountpoint/ä.txt"
test "$(sudo find "$mountpoint" -mindepth 1 -maxdepth 1 -printf '%f\n' | grep -Fx 'ReadMe.TXT' | wc -l)" -eq 1
test "$(sudo find "$mountpoint" -mindepth 1 -maxdepth 1 -printf '%f\n' | grep -E '^(Ä|ä)\.txt$' | wc -l)" -eq 2

printf 'updated through folded lookup\n' | sudo tee "$mountpoint/README.txt" >/dev/null
test "$(sudo cat "$mountpoint/ReadMe.TXT")" = "updated through folded lookup"

sudo sync
sudo umount "$mountpoint"
mounted=0
sudo losetup -d "$loopdev"
loopdev=""

"$build/fsck.infiltratorfs" --scrub "$image" |
    tee /tmp/infilfs-casefold-scrub.txt
grep -Fq 'Result:              CLEAN' /tmp/infilfs-casefold-scrub.txt
"$build/infilfs-tool" "$image" ls / | grep -Fq 'ReadMe.TXT'
printf 'Native case-folded namespace qualification passed.\n'
