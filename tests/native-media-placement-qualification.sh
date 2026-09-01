#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail

if (( $# != 2 )); then
    echo "Usage: $0 <userspace-build-dir> <infiltratorfs.ko>" >&2
    exit 2
fi

BUILD="$(readlink -f "$1")"
MODULE="$(readlink -f "$2")"
for path in "$BUILD/mkfs.infilfs" "$BUILD/infilfs-scrub" "$MODULE"; do
    [[ -e "$path" ]] || { echo "Missing qualification input: $path" >&2; exit 1; }
done

TMP_BASE="${RUNNER_TEMP:-/tmp}"
WORK="$(mktemp -d "$TMP_BASE/infiltratorfs-media.XXXXXX")"
MOUNTPOINT="$WORK/mnt"
LOADED=0
LOOPDEV=""
MOUNTED=0

cleanup() {
    rc=$?
    set +e
    if (( MOUNTED )); then sudo umount -l "$MOUNTPOINT" || true; fi
    if [[ -n "$LOOPDEV" ]]; then sudo losetup -d "$LOOPDEV" || true; fi
    if (( LOADED )); then sudo rmmod infiltratorfs || true; fi
    rm -rf "$WORK"
    exit "$rc"
}
trap cleanup EXIT INT TERM

if lsmod | awk '{print $1}' | grep -qx infiltratorfs; then
    if findmnt -rn -t infiltratorfs >/dev/null 2>&1; then
        echo "Refusing media qualification while another InfiltratorFS mount is active." >&2
        exit 1
    fi
    sudo rmmod infiltratorfs
fi
sudo insmod "$MODULE"
LOADED=1
mkdir -p "$MOUNTPOINT"

run_profile() {
    local profile="$1"
    local image="$WORK/$profile.img"
    local start_marker
    local log
    local scored

    truncate -s 512M "$image"
    sudo env SUDO_UID="$(id -u)" SUDO_GID="$(id -g)"         "$BUILD/mkfs.infilfs" -L "Media-$profile" "$image" >/dev/null
    LOOPDEV="$(sudo losetup --find --show "$image")"
    start_marker="$(date '+%Y-%m-%d %H:%M:%S')"
    sudo mount -t infiltratorfs -o "rw,media=$profile"         "$LOOPDEV" "$MOUNTPOINT"
    MOUNTED=1
    test "$(findmnt -rn -T "$MOUNTPOINT" -o FSTYPE)" = infiltratorfs
    findmnt -rn -T "$MOUNTPOINT" -o OPTIONS | grep -Fq "media=$profile"

    sudo python3 - "$MOUNTPOINT" <<'PY'
import os
import sys

root = sys.argv[1]
seq = os.path.join(root, "sequential.bin")
payload = bytes((i * 37 + 11) & 0xff for i in range(1024 * 1024))
with open(seq, "xb", buffering=0) as stream:
    for _ in range(24):
        stream.write(payload)
    os.fsync(stream.fileno())

fd = os.open(seq, os.O_RDWR | os.O_CLOEXEC)
try:
    size = os.fstat(fd).st_size
    for i in range(96):
        offset = (1 + ((i * 193) % ((size // 4096) - 2))) * 4096
        block = bytes(((i * 29 + j * 13 + 7) & 0xff) for j in range(4096))
        assert os.pwrite(fd, block, offset) == 4096
    os.fsync(fd)
finally:
    os.close(fd)

sparse = os.path.join(root, "direct-sparse.bin")
fd = os.open(sparse, os.O_CREAT | os.O_RDWR | os.O_EXCL, 0o600)
try:
    offset = 32 * 1024 * 1024 + 321
    data = b"media-aware-direct-sparse"
    assert os.pwrite(fd, data, offset) == len(data)
    os.fsync(fd)
    assert os.fstat(fd).st_size == offset + len(data)
    assert os.pread(fd, len(data), offset) == data
finally:
    os.close(fd)
PY

    sync
    sudo umount "$MOUNTPOINT"
    MOUNTED=0
    log="$(sudo dmesg --since "$start_marker" 2>/dev/null |         grep "InfiltratorFS: allocator reservations=.*media=$profile" | tail -n1 || true)"
    printf '%s\n' "$log"
    test -n "$log"
    grep -Fq 'media_source=override' <<<"$log"
    grep -Eq 'workload_seq=[1-9][0-9]*' <<<"$log"
    grep -Eq 'workload_random=[1-9][0-9]*' <<<"$log"
    grep -Eq 'workload_sparse=[1-9][0-9]*' <<<"$log"
    if [[ "$profile" == rotational ]]; then
        scored="$(sed -nE 's/.*media_rotational_scored=([0-9]+).*/\1/p' <<<"$log")"
    else
        scored="$(sed -nE 's/.*media_nonrotational_scored=([0-9]+).*/\1/p' <<<"$log")"
        grep -Eq 'best_fit=[1-9][0-9]*' <<<"$log"
    fi
    test -n "$scored"
    test "$scored" -ge 1

    "$BUILD/infilfs-scrub" "$image" | tee "$WORK/$profile-scrub.txt"
    grep -Fq 'Result:              CLEAN' "$WORK/$profile-scrub.txt"
    sudo losetup -d "$LOOPDEV"
    LOOPDEV=""
}

run_profile rotational
run_profile nonrotational

AUTO_IMAGE="$WORK/auto.img"
truncate -s 128M "$AUTO_IMAGE"
sudo env SUDO_UID="$(id -u)" SUDO_GID="$(id -g)"     "$BUILD/mkfs.infilfs" -L Media-Auto "$AUTO_IMAGE" >/dev/null
LOOPDEV="$(sudo losetup --find --show "$AUTO_IMAGE")"
sudo mount -t infiltratorfs -o rw "$LOOPDEV" "$MOUNTPOINT"
MOUNTED=1
findmnt -rn -T "$MOUNTPOINT" -o OPTIONS |     grep -Eq 'media=(rotational|nonrotational|balanced)'
sudo umount "$MOUNTPOINT"
MOUNTED=0
sudo losetup -d "$LOOPDEV"
LOOPDEV=""

echo "Native media-aware rotational/non-rotational placement qualification: PASS"
