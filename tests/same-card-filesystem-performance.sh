#!/usr/bin/env bash
# InfiltratorFS same-card filesystem performance baseline
#
# Non-destructive to filesystem layout: this DOES NOT FORMAT partitions.
# It creates and removes one temporary benchmark directory on each target.
#
# Targets on the standard InfiltratorFS comparative SD card:
#   ext4          /dev/mmcblk0p8   (LD_EXT4)
#   xfs           /dev/mmcblk0p9   (LD_XFS)
#   btrfs         /dev/mmcblk0p10  (LD_BTRFS)
#   infiltratorfs /dev/mmcblk0p22
#
# The workload deliberately matches the important 0.18.8 native qualification
# mechanics so the numbers can be compared directly:
#   - 512 MiB zero-file write with conv=fsync
#   - SHA-256 verified sequential read
#   - 4,000 deterministic random 4 KiB pwrite() operations
#   - fsync every 50 random writes, plus one final fsync
#   - 200 separate 4 KiB pwrite()+fsync latency samples
#   - 5,000-file create/stat/rename/unlink metadata pass
#
# Run as your normal desktop user. sudo is used only for mount management and
# creating the benchmark directory on filesystem roots that are root-owned.
#
# Optional:
#   ROUNDS=3 ./same-card-filesystem-performance.sh
#
set -Eeuo pipefail

EXPECTED_PARENT="/dev/mmcblk0"
ROUNDS="${ROUNDS:-1}"
SEQ_MIB=512
SEQ_BYTES=$((SEQ_MIB * 1024 * 1024))
RANDOM_WRITES=4000
BLOCK_SIZE=4096
FSYNC_OPS=200
META_FILES=5000
MIN_FREE_BYTES=$((900 * 1024 * 1024))

RESULT_ROOT="${HOME}/infiltratorfs-test-results"
STAMP="$(date +%Y%m%d-%H%M%S)"
RESULT_DIR="${RESULT_ROOT}/same-card-performance-${STAMP}"
CSV="${RESULT_DIR}/results.csv"
LOG="${RESULT_DIR}/full.log"
SUMMARY="${RESULT_DIR}/summary.txt"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/infiltratorfs-fs-perf.XXXXXX")"

mkdir -p "$RESULT_DIR"
touch "$LOG" "$SUMMARY"
exec > >(tee -a "$LOG") 2>&1

declare -a CREATED_MOUNTS=()
declare -a CREATED_DIRS=()

cleanup() {
    local rc=$?
    set +e
    for d in "${CREATED_DIRS[@]:-}"; do
        sudo rm -rf -- "$d" >/dev/null 2>&1 || true
    done
    for ((i=${#CREATED_MOUNTS[@]}-1; i>=0; --i)); do
        sudo umount "${CREATED_MOUNTS[$i]}" >/dev/null 2>&1 || true
        sudo rmdir "${CREATED_MOUNTS[$i]}" >/dev/null 2>&1 || true
    done
    rm -rf "$WORK"
    exit "$rc"
}
trap cleanup EXIT INT TERM

section() {
    printf '\n================================================================\n%s\n================================================================\n' "$1"
}

fatal() {
    printf '[FATAL] %s\n' "$*" >&2
    exit 1
}

need() {
    command -v "$1" >/dev/null 2>&1 || fatal "Required command missing: $1"
}

ns_now() {
    date +%s%N
}

elapsed_s() {
    awk -v a="$1" -v b="$2" 'BEGIN {printf "%.6f", (b-a)/1000000000}'
}

mibps() {
    awk -v mib="$1" -v sec="$2" 'BEGIN {if (sec<=0) print 0; else printf "%.3f", mib/sec}'
}

csv_escape() {
    local s="${1//\"/\"\"}"
    printf '"%s"' "$s"
}

declare -a NAMES=("ext4" "xfs" "btrfs" "infiltratorfs")
declare -A DEV=(
    [ext4]="/dev/mmcblk0p8"
    [xfs]="/dev/mmcblk0p9"
    [btrfs]="/dev/mmcblk0p10"
    [infiltratorfs]="/dev/mmcblk0p22"
)
declare -A EXPECTED_TYPE=(
    [ext4]="ext4"
    [xfs]="xfs"
    [btrfs]="btrfs"
    [infiltratorfs]="infiltratorfs"
)
declare -A EXPECTED_PARTNO=(
    [ext4]="8"
    [xfs]="9"
    [btrfs]="10"
    [infiltratorfs]="22"
)

for c in awk blockdev cmp date dd df find findmnt grep head lsblk mount \
         nproc python3 readlink sha256sum stat sudo sync tr umount; do
    need "$c"
done

[[ "$ROUNDS" =~ ^[1-9][0-9]*$ ]] || fatal "ROUNDS must be a positive integer."

section "SAFETY / TARGET VERIFICATION"

sudo -v

printf 'Results: %s\n' "$RESULT_DIR"
printf 'Rounds:  %s\n' "$ROUNDS"
printf 'Kernel:  %s\n' "$(uname -r)"
printf 'CPU:     %s\n' "$(awk -F: '/model name/ {sub(/^[ \t]+/,"",$2); print $2; exit}' /proc/cpuinfo)"
printf '\n'

for name in "${NAMES[@]}"; do
    dev="${DEV[$name]}"
    expected_type="${EXPECTED_TYPE[$name]}"
    expected_part="${EXPECTED_PARTNO[$name]}"

    [[ -b "$dev" ]] || fatal "$dev is not a block device."
    parent="$(lsblk -ndo PKNAME "$dev" | head -n1 | tr -d '[:space:]')"
    part="$(lsblk -ndo PARTN "$dev" | head -n1 | tr -d '[:space:]')"
    fstype="$(lsblk -ndo FSTYPE "$dev" | head -n1 | tr -d '[:space:]')"

    [[ "/dev/$parent" == "$EXPECTED_PARENT" ]] ||
        fatal "$dev parent is /dev/$parent, expected $EXPECTED_PARENT."
    [[ "$part" == "$expected_part" ]] ||
        fatal "$dev partition is '$part', expected '$expected_part'."
    [[ "$fstype" == "$expected_type" ]] ||
        fatal "$dev filesystem is '$fstype', expected '$expected_type'."

    printf '%-14s %-16s %-14s %s bytes\n' \
        "$name" "$dev" "$fstype" "$(blockdev --getsize64 "$dev")"
done

printf 'filesystem,round,device,mount_options,seq_write_seconds,seq_write_mib_s,seq_read_seconds,seq_read_mib_s,random_seconds,random_iops,fsync_mean_ms,fsync_p50_ms,fsync_p95_ms,fsync_p99_ms,fsync_max_ms,meta_create_ops_s,meta_stat_ops_s,meta_rename_ops_s,meta_unlink_ops_s\n' > "$CSV"

# Build the deterministic post-random-write reference image once, outside all
# tested filesystems. This is correctness-only work and is not timed.
section "BUILD RANDOM-WRITE REFERENCE HASH"
reference="$WORK/random-reference.bin"
truncate -s "$SEQ_BYTES" "$reference"
python3 - "$reference" "$RANDOM_WRITES" "$BLOCK_SIZE" <<'PY'
import os, random, sys
path, count, block = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
size = os.path.getsize(path)
rng = random.Random(0x1F51A7E)
fd = os.open(path, os.O_RDWR)
try:
    for iteration in range(count):
        offset = rng.randrange(size // block) * block
        payload = bytes(((iteration * 17 + i * 31 + 9) & 0xff)
                        for i in range(block))
        assert os.pwrite(fd, payload, offset) == block
finally:
    os.close(fd)
PY
REFERENCE_HASH="$(sha256sum "$reference" | awk '{print $1}')"
ZERO_HASH="$(head -c "$SEQ_BYTES" /dev/zero | sha256sum | awk '{print $1}')"
printf 'Zero image SHA-256:   %s\n' "$ZERO_HASH"
printf 'Random result SHA-256: %s\n' "$REFERENCE_HASH"

MOUNTPOINT_RESULT=""

get_or_mount() {
    local name="$1"
    local dev="${DEV[$name]}"
    local fstype="${EXPECTED_TYPE[$name]}"
    local mp

    MOUNTPOINT_RESULT=""
    mp="$(findmnt -rn -S "$dev" -o TARGET | head -n1 || true)"
    if [[ -n "$mp" ]]; then
        local opts
        opts="$(findmnt -rn -T "$mp" -o OPTIONS)"
        [[ ",$opts," == *,rw,* ]] || fatal "$dev is already mounted read-only at $mp."
        MOUNTPOINT_RESULT="$mp"
        return 0
    fi

    mp="/mnt/infiltratorfs-perf-${name}"
    sudo mkdir -p "$mp"
    if [[ "$fstype" == "infiltratorfs" ]]; then
        sudo mount -t infiltratorfs "$dev" "$mp"
    else
        sudo mount -t "$fstype" "$dev" "$mp"
    fi
    CREATED_MOUNTS+=("$mp")
    MOUNTPOINT_RESULT="$mp"
}

benchmark_one() {
    local name="$1"
    local round="$2"
    local dev="${DEV[$name]}"
    local mp="$3"
    local bench="${mp}/.infiltratorfs-perf-${STAMP}-r${round}"
    local t1 t2 sec
    local seq_write_s seq_write_rate seq_read_s seq_read_rate
    local random_s random_iops
    local fs_mean fs_p50 fs_p95 fs_p99 fs_max
    local meta_create meta_stat meta_rename meta_unlink
    local mount_opts free_bytes

    mount_opts="$(findmnt -rn -T "$mp" -o OPTIONS)"
    free_bytes="$(df -B1 --output=avail "$mp" | tail -n1 | tr -d '[:space:]')"
    (( free_bytes >= MIN_FREE_BYTES )) ||
        fatal "$name has only $free_bytes free bytes; need at least $MIN_FREE_BYTES."

    sudo mkdir "$bench"
    sudo chown "$(id -u):$(id -g)" "$bench"
    CREATED_DIRS+=("$bench")

    section "$name round $round — 512 MiB sequential write"
    sync
    t1="$(ns_now)"
    dd if=/dev/zero of="$bench/large-512m.bin" bs=4M count=128 conv=fsync status=progress
    t2="$(ns_now)"
    seq_write_s="$(elapsed_s "$t1" "$t2")"
    seq_write_rate="$(mibps "$SEQ_MIB" "$seq_write_s")"
    printf '[PERF] %s sequential write: %s MiB/s (%s s)\n' \
        "$name" "$seq_write_rate" "$seq_write_s"

    [[ "$(stat -c '%s' "$bench/large-512m.bin")" == "$SEQ_BYTES" ]] ||
        fatal "$name sequential file size mismatch."

    section "$name round $round — SHA-256 verified sequential read"
    t1="$(ns_now)"
    got_hash="$(sha256sum "$bench/large-512m.bin" | awk '{print $1}')"
    t2="$(ns_now)"
    [[ "$got_hash" == "$ZERO_HASH" ]] || fatal "$name zero-file SHA-256 mismatch."
    seq_read_s="$(elapsed_s "$t1" "$t2")"
    seq_read_rate="$(mibps "$SEQ_MIB" "$seq_read_s")"
    printf '[PERF] %s verified sequential read: %s MiB/s (%s s)\n' \
        "$name" "$seq_read_rate" "$seq_read_s"

    section "$name round $round — 4,000 random 4 KiB overwrites"
    random_result="$WORK/random-${name}-${round}.txt"
    python3 - "$bench/large-512m.bin" "$RANDOM_WRITES" "$BLOCK_SIZE" > "$random_result" <<'PY'
import os, random, sys, time
path, count, block = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
size = os.path.getsize(path)
rng = random.Random(0x1F51A7E)
fd = os.open(path, os.O_RDWR)
start = time.perf_counter()
try:
    for iteration in range(count):
        offset = rng.randrange(size // block) * block
        payload = bytes(((iteration * 17 + i * 31 + 9) & 0xff)
                        for i in range(block))
        assert os.pwrite(fd, payload, offset) == block
        if iteration % 50 == 49:
            os.fsync(fd)
    os.fsync(fd)
finally:
    os.close(fd)
elapsed = time.perf_counter() - start
print(f"seconds={elapsed:.6f}")
print(f"iops={count/elapsed:.6f}")
PY
    random_s="$(awk -F= '$1=="seconds"{print $2}' "$random_result")"
    random_iops="$(awk -F= '$1=="iops"{print $2}' "$random_result")"
    got_hash="$(sha256sum "$bench/large-512m.bin" | awk '{print $1}')"
    [[ "$got_hash" == "$REFERENCE_HASH" ]] ||
        fatal "$name random-overwrite result SHA-256 mismatch."
    printf '[PERF] %s random 4 KiB overwrite: %.2f IOPS (%s s)\n' \
        "$name" "$random_iops" "$random_s"

    section "$name round $round — 200 pwrite()+fsync samples"
    fsync_result="$WORK/fsync-${name}-${round}.txt"
    python3 - "$bench/fsync-publications.bin" "$FSYNC_OPS" > "$fsync_result" <<'PY'
import math, os, statistics, sys, time
path, count = sys.argv[1], int(sys.argv[2])
lat = []
fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_EXCL, 0o600)
try:
    for iteration in range(count):
        payload = bytes(((iteration * 23 + i * 7 + 3) & 0xff)
                        for i in range(4096))
        assert os.pwrite(fd, payload, iteration * 4096) == len(payload)
        t = time.perf_counter_ns()
        os.fsync(fd)
        lat.append((time.perf_counter_ns() - t) / 1e6)
finally:
    os.close(fd)
lat.sort()
def pct(p):
    return lat[max(0, min(len(lat)-1, math.ceil(p*len(lat))-1))]
print(f"mean={statistics.mean(lat):.6f}")
print(f"p50={pct(.50):.6f}")
print(f"p95={pct(.95):.6f}")
print(f"p99={pct(.99):.6f}")
print(f"max={max(lat):.6f}")
PY
    fs_mean="$(awk -F= '$1=="mean"{print $2}' "$fsync_result")"
    fs_p50="$(awk -F= '$1=="p50"{print $2}' "$fsync_result")"
    fs_p95="$(awk -F= '$1=="p95"{print $2}' "$fsync_result")"
    fs_p99="$(awk -F= '$1=="p99"{print $2}' "$fsync_result")"
    fs_max="$(awk -F= '$1=="max"{print $2}' "$fsync_result")"
    printf '[PERF] %s fsync: mean=%.3fms p50=%.3fms p95=%.3fms p99=%.3fms max=%.3fms\n' \
        "$name" "$fs_mean" "$fs_p50" "$fs_p95" "$fs_p99" "$fs_max"

    section "$name round $round — 5,000-file metadata workload"
    meta_result="$WORK/meta-${name}-${round}.txt"
    python3 - "$bench" "$META_FILES" > "$meta_result" <<'PY'
import os, sys, time
base, count = sys.argv[1], int(sys.argv[2])
root = os.path.join(base, "metadata")
os.mkdir(root)

def measure(fn):
    t = time.perf_counter()
    fn()
    return time.perf_counter() - t

def create():
    for i in range(count):
        fd = os.open(os.path.join(root, f"f-{i:05d}"),
                     os.O_CREAT | os.O_WRONLY | os.O_EXCL, 0o600)
        os.close(fd)

def stat_all():
    for i in range(count):
        os.stat(os.path.join(root, f"f-{i:05d}"))

def rename_all():
    for i in range(count):
        os.rename(os.path.join(root, f"f-{i:05d}"),
                  os.path.join(root, f"r-{i:05d}"))

def unlink_all():
    for i in range(count):
        os.unlink(os.path.join(root, f"r-{i:05d}"))

create_s = measure(create)
stat_s = measure(stat_all)
rename_s = measure(rename_all)
unlink_s = measure(unlink_all)
os.rmdir(root)

print(f"create={count/create_s:.6f}")
print(f"stat={count/stat_s:.6f}")
print(f"rename={count/rename_s:.6f}")
print(f"unlink={count/unlink_s:.6f}")
PY
    meta_create="$(awk -F= '$1=="create"{print $2}' "$meta_result")"
    meta_stat="$(awk -F= '$1=="stat"{print $2}' "$meta_result")"
    meta_rename="$(awk -F= '$1=="rename"{print $2}' "$meta_result")"
    meta_unlink="$(awk -F= '$1=="unlink"{print $2}' "$meta_result")"
    printf '[PERF] %s metadata ops/s: create=%.1f stat=%.1f rename=%.1f unlink=%.1f\n' \
        "$name" "$meta_create" "$meta_stat" "$meta_rename" "$meta_unlink"

    printf '%s,%s,%s,' "$name" "$round" "$dev" >> "$CSV"
    csv_escape "$mount_opts" >> "$CSV"
    printf ',%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$seq_write_s" "$seq_write_rate" "$seq_read_s" "$seq_read_rate" \
        "$random_s" "$random_iops" \
        "$fs_mean" "$fs_p50" "$fs_p95" "$fs_p99" "$fs_max" \
        "$meta_create" "$meta_stat" "$meta_rename" "$meta_unlink" >> "$CSV"

    sync
    sudo rm -rf -- "$bench"
    # Remove this path from trap cleanup now that it is gone.
    CREATED_DIRS=("${CREATED_DIRS[@]/$bench}")
}

section "RUN SAME-CARD BASELINE"

for ((round=1; round<=ROUNDS; ++round)); do
    for name in "${NAMES[@]}"; do
        get_or_mount "$name"
        benchmark_one "$name" "$round" "$MOUNTPOINT_RESULT"
    done
done

section "SUMMARY"

python3 - "$CSV" | tee "$SUMMARY" <<'PY'
import csv, statistics, sys
path = sys.argv[1]
rows = list(csv.DictReader(open(path, newline="")))

metrics = [
    ("seq_write_mib_s", "Seq write", "MiB/s", True),
    ("seq_read_mib_s", "Seq read", "MiB/s", True),
    ("random_iops", "Random 4K", "IOPS", True),
    ("fsync_mean_ms", "fsync mean", "ms", False),
    ("fsync_p95_ms", "fsync p95", "ms", False),
    ("meta_create_ops_s", "Create", "ops/s", True),
    ("meta_stat_ops_s", "Stat", "ops/s", True),
    ("meta_rename_ops_s", "Rename", "ops/s", True),
    ("meta_unlink_ops_s", "Unlink", "ops/s", True),
]
names = []
for r in rows:
    if r["filesystem"] not in names:
        names.append(r["filesystem"])

print("InfiltratorFS same-card performance baseline")
print("=" * 52)
for name in names:
    mine = [r for r in rows if r["filesystem"] == name]
    print(f"\n{name} ({mine[0]['device']})")
    for key, label, unit, higher in metrics:
        values = [float(r[key]) for r in mine]
        val = statistics.mean(values)
        print(f"  {label:<14} {val:>12.3f} {unit}")

if "infiltratorfs" in names:
    inf = [r for r in rows if r["filesystem"] == "infiltratorfs"]
    print("\nInfiltratorFS relative to mature filesystems")
    print("-" * 52)
    for other in ("ext4", "xfs", "btrfs"):
        oth = [r for r in rows if r["filesystem"] == other]
        if not oth:
            continue
        print(f"\nvs {other}:")
        for key, label, unit, higher in metrics:
            a = statistics.mean(float(r[key]) for r in inf)
            b = statistics.mean(float(r[key]) for r in oth)
            if not b:
                continue
            if higher:
                ratio = a / b
                print(f"  {label:<14} {ratio:>8.3f}x ({ratio*100:6.1f}%)")
            else:
                ratio = a / b
                print(f"  {label:<14} {ratio:>8.3f}x latency")
PY

printf '\nFull log: %s\n' "$LOG"
printf 'CSV:      %s\n' "$CSV"
printf 'Summary:  %s\n' "$SUMMARY"
printf '\nPASS: all four filesystems completed the same-card workload and all data-integrity checks matched.\n'
