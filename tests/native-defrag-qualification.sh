#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail

if (( $# != 2 )); then
    echo "Usage: $0 <userspace-build-dir> <infiltratorfs.ko>" >&2
    exit 2
fi

BUILD="$(readlink -f "$1")"
MODULE="$(readlink -f "$2")"

for path in "$BUILD/mkfs.infilfs" "$BUILD/infilfs-scrub" "$BUILD/infilfs-tool" "$BUILD/infilfs-optimize" "$MODULE"; do
    [[ -e "$path" ]] || { echo "Missing qualification input: $path" >&2; exit 1; }
done

TMP_BASE="${RUNNER_TEMP:-/tmp}"
WORK="$(mktemp -d "$TMP_BASE/infiltratorfs-defrag.XXXXXX")"
IMAGE="$WORK/defrag.img"
MOUNTPOINT="$WORK/mnt"
LOOPDEV=""
MOUNTED=0
LOADED=0
START_TIME="$(date '+%Y-%m-%d %H:%M:%S')"

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
        echo "Refusing qualification while another InfiltratorFS mount is active." >&2
        exit 1
    fi
    sudo rmmod infiltratorfs
fi
sudo modprobe lz4_compress 2>/dev/null || true
sudo modprobe lz4_decompress 2>/dev/null || true
if ! sudo insmod "$MODULE"; then
    sudo dmesg | tail -n 80 >&2 || true
    exit 1
fi
LOADED=1
grep -qw infiltratorfs /proc/filesystems

mkdir -p "$MOUNTPOINT"
truncate -s 2G "$IMAGE"
sudo env SUDO_UID="$(id -u)" SUDO_GID="$(id -g)"     "$BUILD/mkfs.infilfs" -L NativeDefrag "$IMAGE"
LOOPDEV="$(sudo losetup --direct-io=on --find --show "$IMAGE")"
sudo mount -t infiltratorfs -o rw "$LOOPDEV" "$MOUNTPOINT"
MOUNTED=1
test "$(findmnt -rn -T "$MOUNTPOINT" -o FSTYPE)" = infiltratorfs

FILE="$MOUNTPOINT/fragmented.bin"
LINK="$MOUNTPOINT/fragmented-link.bin"
python3 - "$FILE" <<'PY'
import hashlib
import os
import sys

path = sys.argv[1]
size = 32 * 1024 * 1024
motif = bytes((i * 17 + 3) & 0xff for i in range(256))
chunk = motif * (1024 * 1024 // len(motif))
with open(path, "xb", buffering=0) as stream:
    left = size
    while left:
        data = chunk if left >= len(chunk) else chunk[:left]
        stream.write(data)
        left -= len(data)
    os.fsync(stream.fileno())

# Force many small CoW relocations after the sequential baseline is durable.
fd = os.open(path, os.O_RDWR | os.O_CLOEXEC)
try:
    blocks = size // 4096
    for i in range(96):
        block = 32 + ((i * 257) % (blocks - 64))
        payload = bytes(((i * 31 + j * 7 + 11) & 0xff) for j in range(4096))
        assert os.pwrite(fd, payload, block * 4096) == len(payload)
        os.fsync(fd)
finally:
    os.close(fd)

with open(path, "rb", buffering=0) as stream:
    digest = hashlib.sha256()
    while True:
        data = stream.read(1024 * 1024)
        if not data:
            break
        digest.update(data)
print(digest.hexdigest())
PY

EXPECTED="$(python3 - "$FILE" <<'PY'
import hashlib, sys
h = hashlib.sha256()
with open(sys.argv[1], 'rb', buffering=0) as f:
    while True:
        b = f.read(1024 * 1024)
        if not b:
            break
        h.update(b)
print(h.hexdigest())
PY
)"

# Build several independent highly-compressible EOF clusters while unrelated
# allocations sit between them. Deleting the fillers leaves codec streams
# logically adjacent but physically split.
COMPRESSED="$MOUNTPOINT/compressed-fragmented.bin"
python3 - "$COMPRESSED" "$MOUNTPOINT" <<'PY'
import hashlib
import os
import sys

path, root = sys.argv[1], sys.argv[2]
chunk = (b"InfiltratorFS-compressed-defrag-" * 8192)[:256 * 1024]
with open(path, "xb", buffering=0) as stream:
    for i in range(12):
        stream.write(chunk)
        os.fsync(stream.fileno())
        filler = os.path.join(root, f".defrag-filler-{i}")
        with open(filler, "xb", buffering=0) as f:
            for j in range(256):
                f.write(hashlib.sha256(f"{i}:{j}".encode()).digest() * 128)
            os.fsync(f.fileno())
for i in range(12):
    os.unlink(os.path.join(root, f".defrag-filler-{i}"))
PY
COMPRESSED_EXPECTED="$(sha256sum "$COMPRESSED" | awk '{print $1}')"
COMPRESSED_BLOCKS_BEFORE="$(stat -c '%b' "$COMPRESSED")"
COMPRESSED_METRICS_BEFORE="$("$BUILD/infilfs-optimize" --metrics "$COMPRESSED")"
printf '%s
' "$COMPRESSED_METRICS_BEFORE"
COMPRESSED_EXTENTS="$(sed -nE 's/.*extents=([0-9]+).*/\1/p' <<<"$COMPRESSED_METRICS_BEFORE" | head -n1)"
COMPRESSED_PHYSICAL_RUNS="$(sed -nE 's/.*physical-runs=([0-9]+).*/\1/p' <<<"$COMPRESSED_METRICS_BEFORE" | head -n1)"
[[ "$COMPRESSED_EXTENTS" =~ ^[0-9]+$ ]]
[[ "$COMPRESSED_PHYSICAL_RUNS" =~ ^[0-9]+$ ]]
(( COMPRESSED_EXTENTS > 1 ))
(( COMPRESSED_PHYSICAL_RUNS >= 1 ))

setfattr -n user.infiltratorfs-defrag -v preserved "$FILE"
ln "$FILE" "$LINK"
INO_BEFORE="$(stat -c '%i' "$FILE")"
MTIME_BEFORE="$(stat -c '%y' "$FILE")"
CTIME_BEFORE="$(stat -c '%z' "$FILE")"

echo "=== Fragmentation metrics before ==="
BEFORE="$("$BUILD/infilfs-optimize" --metrics "$FILE")"
printf '%s\n' "$BEFORE"
BEFORE_EXTENTS="$(sed -nE 's/.*extents=([0-9]+).*/\1/p' <<<"$BEFORE" | head -n1)"
[[ "$BEFORE_EXTENTS" =~ ^[0-9]+$ ]]
(( BEFORE_EXTENTS > 2 ))

# Retain the physically fragmented generation before relocation.  The later
# snapshot-cat check proves that old data blocks were not reclaimed out from
# under a retained generation.
sync
sudo umount "$MOUNTPOINT"
MOUNTED=0
"$BUILD/infilfs-tool" "$IMAGE" snapshot-create before-defrag
sudo mount -t infiltratorfs -o rw "$LOOPDEV" "$MOUNTPOINT"
MOUNTED=1
test "$(sha256sum "$FILE" | awk '{print $1}')" = "$EXPECTED"
test "$(stat -c '%i' "$FILE")" = "$INO_BEFORE"
test "$(getfattr --only-values -n user.infiltratorfs-defrag "$FILE")" = preserved

echo "=== Online defrag ==="
"$BUILD/infilfs-optimize" --defrag --max-mib 64 --passes 64 "$FILE"

echo "=== Fragmentation metrics after ==="
AFTER="$("$BUILD/infilfs-optimize" --metrics "$FILE")"
printf '%s\n' "$AFTER"
AFTER_EXTENTS="$(sed -nE 's/.*extents=([0-9]+).*/\1/p' <<<"$AFTER" | head -n1)"
[[ "$AFTER_EXTENTS" =~ ^[0-9]+$ ]]
(( AFTER_EXTENTS < BEFORE_EXTENTS ))

echo "=== Compressed online defrag ==="
COMPRESSED_DEFRAG="$("$BUILD/infilfs-optimize" --defrag --max-mib 64 --passes 16 "$COMPRESSED")"
printf '%s
' "$COMPRESSED_DEFRAG"
COMPRESSED_METRICS_AFTER="$("$BUILD/infilfs-optimize" --metrics "$COMPRESSED")"
printf '%s
' "$COMPRESSED_METRICS_AFTER"
COMPRESSED_PHYSICAL_RUNS_AFTER="$(sed -nE 's/.*physical-runs=([0-9]+).*/\1/p' <<<"$COMPRESSED_METRICS_AFTER" | head -n1)"
[[ "$COMPRESSED_PHYSICAL_RUNS_AFTER" =~ ^[0-9]+$ ]]
if (( COMPRESSED_PHYSICAL_RUNS > 1 )); then
    grep -Eq 'moved 0\.[0-9]*[1-9][0-9]* MiB|moved [1-9][0-9]*\.[0-9]+ MiB' <<<"$COMPRESSED_DEFRAG"
    (( COMPRESSED_PHYSICAL_RUNS_AFTER < COMPRESSED_PHYSICAL_RUNS ))
else
    # Codec boundaries are not physical fragmentation. If placement already
    # made every stored stream contiguous, defrag must avoid pointless CoW.
    grep -Fq 'moved 0.00 MiB' <<<"$COMPRESSED_DEFRAG"
    (( COMPRESSED_PHYSICAL_RUNS_AFTER == 1 ))
fi
test "$(sha256sum "$COMPRESSED" | awk '{print $1}')" = "$COMPRESSED_EXPECTED"
test "$(stat -c '%b' "$COMPRESSED")" = "$COMPRESSED_BLOCKS_BEFORE"

ACTUAL="$(sha256sum "$FILE" | awk '{print $1}')"
test "$ACTUAL" = "$EXPECTED"
test "$(sha256sum "$LINK" | awk '{print $1}')" = "$EXPECTED"
test "$(stat -c '%i' "$FILE")" = "$INO_BEFORE"
test "$(stat -c '%i' "$LINK")" = "$INO_BEFORE"
test "$(getfattr --only-values -n user.infiltratorfs-defrag "$FILE")" = preserved

# Defrag is physical optimisation, not user-visible content modification.
test "$(stat -c '%y' "$FILE")" = "$MTIME_BEFORE"
test "$(stat -c '%z' "$FILE")" = "$CTIME_BEFORE"

sync
sudo umount "$MOUNTPOINT"
MOUNTED=0

SCRUB="$WORK/scrub.txt"
"$BUILD/infilfs-scrub" "$IMAGE" | tee "$SCRUB"
grep -Fq 'Result:              CLEAN' "$SCRUB"

SNAPSHOT_COPY="$WORK/snapshot-before-defrag.bin"
"$BUILD/infilfs-tool" "$IMAGE" snapshot-cat before-defrag /fragmented.bin > "$SNAPSHOT_COPY"
test "$(sha256sum "$SNAPSHOT_COPY" | awk '{print $1}')" = "$EXPECTED"
COMPRESSED_SNAPSHOT="$WORK/snapshot-compressed-before-defrag.bin"
"$BUILD/infilfs-tool" "$IMAGE" snapshot-cat before-defrag /compressed-fragmented.bin > "$COMPRESSED_SNAPSHOT"
test "$(sha256sum "$COMPRESSED_SNAPSHOT" | awk '{print $1}')" = "$COMPRESSED_EXPECTED"

sudo mount -t infiltratorfs -o ro "$LOOPDEV" "$MOUNTPOINT"
MOUNTED=1
test "$(sha256sum "$FILE" | awk '{print $1}')" = "$EXPECTED"
test "$(sha256sum "$LINK" | awk '{print $1}')" = "$EXPECTED"
test "$(sha256sum "$COMPRESSED" | awk '{print $1}')" = "$COMPRESSED_EXPECTED"
test "$(stat -c '%b' "$COMPRESSED")" = "$COMPRESSED_BLOCKS_BEFORE"
test "$(getfattr --only-values -n user.infiltratorfs-defrag "$FILE")" = preserved
"$BUILD/infilfs-optimize" --metrics "$FILE"

sudo umount "$MOUNTPOINT"
MOUNTED=0

FAILURES="$(sudo dmesg --since "$START_TIME" 2>/dev/null |     grep -E 'EUCLEAN|Structure needs cleaning|BUG:|Oops:|Kernel panic|hung task|soft lockup|hard LOCKUP|general protection fault' || true)"
if [[ -n "$FAILURES" ]]; then
    echo "$FAILURES" >&2
    exit 1
fi

echo "Native fragmentation metrics and online defragmentation qualification: PASS"
