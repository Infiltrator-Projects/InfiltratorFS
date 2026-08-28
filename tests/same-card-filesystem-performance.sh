#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Same-card ext4/XFS/Btrfs/InfiltratorFS benchmark.
# Does NOT format partitions. It creates then removes temporary test data.
set -Eeuo pipefail

ROUNDS="${ROUNDS:-1}"
PARENT="/dev/mmcblk0"
SEQ_MIB=512
SEQ_BYTES=$((SEQ_MIB * 1024 * 1024))
RANDOM_WRITES=4000
FSYNC_OPS=200
META_FILES=5000
MIN_FREE=$((900 * 1024 * 1024))

declare -a NAMES=(ext4 xfs btrfs infiltratorfs)
declare -A DEV=(
  [ext4]="/dev/mmcblk0p8"
  [xfs]="/dev/mmcblk0p9"
  [btrfs]="/dev/mmcblk0p10"
  [infiltratorfs]="/dev/mmcblk0p22"
)
declare -A PART=([ext4]=8 [xfs]=9 [btrfs]=10 [infiltratorfs]=22)
declare -A TYPE=([ext4]=ext4 [xfs]=xfs [btrfs]=btrfs [infiltratorfs]=infiltratorfs)

STAMP="$(date +%Y%m%d-%H%M%S)"
RESULT_DIR="$HOME/infiltratorfs-test-results/same-card-performance-$STAMP"
CSV="$RESULT_DIR/results.csv"
LOG="$RESULT_DIR/full.log"
SUMMARY="$RESULT_DIR/summary.txt"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/infiltratorfs-perf.XXXXXX")"
declare -a OUR_MOUNTS=()
declare -a OUR_DIRS=()

mkdir -p "$RESULT_DIR"
exec > >(tee -a "$LOG") 2>&1

die() { printf '[FATAL] %s\n' "$*" >&2; exit 1; }
section() { printf '\n================================================================\n%s\n================================================================\n' "$1"; }

cleanup() {
  local rc=$?
  set +e
  for d in "${OUR_DIRS[@]:-}"; do [[ -n "$d" ]] && sudo rm -rf -- "$d"; done
  for ((i=${#OUR_MOUNTS[@]}-1; i>=0; --i)); do
    sudo umount "${OUR_MOUNTS[$i]}" || true
    sudo rmdir "${OUR_MOUNTS[$i]}" || true
  done
  rm -rf "$TMP"
  exit "$rc"
}
trap cleanup EXIT INT TERM

for c in awk blockdev date dd df findmnt head lsblk mount python3 sha256sum \
         stat sudo sync tr truncate umount uname; do
  command -v "$c" >/dev/null || die "Missing command: $c"
done
[[ "$ROUNDS" =~ ^[1-9][0-9]*$ ]] || die "ROUNDS must be a positive integer."

section "SAFETY / TARGET VERIFICATION"
sudo -v
printf 'Results: %s\nRounds: %s\nKernel: %s\n' "$RESULT_DIR" "$ROUNDS" "$(uname -r)"

for n in "${NAMES[@]}"; do
  d="${DEV[$n]}"
  [[ -b "$d" ]] || die "$d is not a block device"
  parent="$(lsblk -ndo PKNAME "$d" | head -1 | tr -d '[:space:]')"
  part="$(lsblk -ndo PARTN "$d" | head -1 | tr -d '[:space:]')"
  type="$(lsblk -ndo FSTYPE "$d" | head -1 | tr -d '[:space:]')"
  [[ "/dev/$parent" == "$PARENT" ]] || die "$d parent is /dev/$parent, expected $PARENT"
  [[ "$part" == "${PART[$n]}" ]] || die "$d partition is '$part', expected '${PART[$n]}'"
  [[ "$type" == "${TYPE[$n]}" ]] || die "$d filesystem is '$type', expected '${TYPE[$n]}'"
  printf '%-14s %-16s %-14s %s bytes\n' \
    "$n" "$d" "$type" "$(sudo blockdev --getsize64 "$d")"
done

printf '%s\n' \
'filesystem,round,device,mount_options,seq_write_seconds,seq_write_mib_s,seq_read_seconds,seq_read_mib_s,random_seconds,random_iops,fsync_mean_ms,fsync_p50_ms,fsync_p95_ms,fsync_p99_ms,fsync_max_ms,meta_create_ops_s,meta_stat_ops_s,meta_rename_ops_s,meta_unlink_ops_s' > "$CSV"

section "BUILD CORRECTNESS REFERENCE"
REFERENCE="$TMP/reference.bin"
truncate -s "$SEQ_BYTES" "$REFERENCE"
python3 - "$REFERENCE" "$RANDOM_WRITES" <<'PY'
import os, random, sys
path, count = sys.argv[1], int(sys.argv[2])
rng = random.Random(0x1F51A7E)
size = os.path.getsize(path)
fd = os.open(path, os.O_RDWR)
try:
    for i in range(count):
        off = rng.randrange(size // 4096) * 4096
        payload = bytes(((i * 17 + j * 31 + 9) & 0xff) for j in range(4096))
        assert os.pwrite(fd, payload, off) == 4096
finally:
    os.close(fd)
PY
REFERENCE_HASH="$(sha256sum "$REFERENCE" | awk '{print $1}')"
ZERO_HASH="$(head -c "$SEQ_BYTES" /dev/zero | sha256sum | awk '{print $1}')"
printf 'Zero SHA-256:   %s\nRandom SHA-256: %s\n' "$ZERO_HASH" "$REFERENCE_HASH"

MOUNT_RESULT=""
get_mount() {
  local n="$1" d="${DEV[$1]}" t="${TYPE[$1]}" mp opts
  MOUNT_RESULT=""
  mp="$(findmnt -rn -S "$d" -o TARGET | head -1 || true)"
  if [[ -n "$mp" ]]; then
    opts="$(findmnt -rn -T "$mp" -o OPTIONS)"
    [[ ",$opts," == *,rw,* ]] || die "$d is mounted read-only at $mp"
    MOUNT_RESULT="$mp"
    return
  fi
  mp="/mnt/infiltratorfs-perf-$n"
  sudo mkdir -p "$mp"
  sudo mount -t "$t" "$d" "$mp"
  OUR_MOUNTS+=("$mp")
  MOUNT_RESULT="$mp"
}

benchmark() {
  local n="$1" round="$2" mp="$3" d="${DEV[$1]}"
  local dir="$mp/.infiltratorfs-perf-$STAMP-r$round"
  local opts free write_out read_out random_out fsync_out meta_out

  opts="$(findmnt -rn -T "$mp" -o OPTIONS)"
  free="$(df -B1 --output=avail "$mp" | tail -1 | tr -d '[:space:]')"
  (( free >= MIN_FREE )) || die "$n has only $free free bytes"

  sudo mkdir "$dir"
  sudo chown "$(id -u):$(id -g)" "$dir"
  OUR_DIRS+=("$dir")

  section "$n round $round — sequential write/read"
  write_out="$(python3 - "$dir/large.bin" "$SEQ_MIB" <<'PY'
import subprocess, sys, time
path, mib = sys.argv[1], int(sys.argv[2])
t=time.perf_counter()
subprocess.run(["dd","if=/dev/zero",f"of={path}","bs=4M",f"count={mib//4}","conv=fsync","status=progress"],check=True)
s=time.perf_counter()-t
print(f"{s:.6f} {mib/s:.6f}")
PY
)"
  write_s="${write_out%% *}"; write_rate="${write_out##* }"
  [[ "$(stat -c '%s' "$dir/large.bin")" == "$SEQ_BYTES" ]] || die "$n sequential size mismatch"

  read_out="$(python3 - "$dir/large.bin" "$SEQ_MIB" "$ZERO_HASH" <<'PY'
import hashlib, sys, time
path, mib, expected = sys.argv[1], int(sys.argv[2]), sys.argv[3]
h=hashlib.sha256(); t=time.perf_counter()
with open(path,'rb',buffering=0) as f:
    while True:
        b=f.read(1024*1024)
        if not b: break
        h.update(b)
s=time.perf_counter()-t
assert h.hexdigest()==expected
print(f"{s:.6f} {mib/s:.6f}")
PY
)"
  read_s="${read_out%% *}"; read_rate="${read_out##* }"
  printf '[PERF] %s seq write %.2f MiB/s; verified read %.2f MiB/s\n' \
    "$n" "$write_rate" "$read_rate"

  section "$n round $round — 4,000 random 4 KiB overwrites"
  random_out="$(python3 - "$dir/large.bin" "$RANDOM_WRITES" <<'PY'
import os, random, sys, time
path, count = sys.argv[1], int(sys.argv[2])
rng=random.Random(0x1F51A7E); size=os.path.getsize(path)
fd=os.open(path,os.O_RDWR); t=time.perf_counter()
try:
    for i in range(count):
        off=rng.randrange(size//4096)*4096
        payload=bytes(((i*17+j*31+9)&0xff) for j in range(4096))
        assert os.pwrite(fd,payload,off)==4096
        if i%50==49: os.fsync(fd)
    os.fsync(fd)
finally:
    os.close(fd)
s=time.perf_counter()-t
print(f"{s:.6f} {count/s:.6f}")
PY
)"
  random_s="${random_out%% *}"; random_iops="${random_out##* }"
  [[ "$(sha256sum "$dir/large.bin" | awk '{print $1}')" == "$REFERENCE_HASH" ]] || die "$n random-write hash mismatch"
  printf '[PERF] %s random 4K: %.2f IOPS\n' "$n" "$random_iops"

  section "$n round $round — 200 pwrite()+fsync samples"
  fsync_out="$(python3 - "$dir/fsync.bin" "$FSYNC_OPS" <<'PY'
import math, os, statistics, sys, time
path,count=sys.argv[1],int(sys.argv[2]); lat=[]
fd=os.open(path,os.O_CREAT|os.O_RDWR|os.O_EXCL,0o600)
try:
    for i in range(count):
        p=bytes(((i*23+j*7+3)&0xff) for j in range(4096))
        assert os.pwrite(fd,p,i*4096)==4096
        t=time.perf_counter_ns(); os.fsync(fd); lat.append((time.perf_counter_ns()-t)/1e6)
finally: os.close(fd)
lat.sort()
def pct(p): return lat[max(0,min(len(lat)-1,math.ceil(p*len(lat))-1))]
print(f"{statistics.mean(lat):.6f} {pct(.5):.6f} {pct(.95):.6f} {pct(.99):.6f} {max(lat):.6f}")
PY
)"
  read -r fs_mean fs_p50 fs_p95 fs_p99 fs_max <<<"$fsync_out"
  printf '[PERF] %s fsync mean %.3f ms; p95 %.3f ms; p99 %.3f ms; max %.3f ms\n' \
    "$n" "$fs_mean" "$fs_p95" "$fs_p99" "$fs_max"

  section "$n round $round — 5,000-file metadata workload"
  meta_out="$(python3 - "$dir" "$META_FILES" <<'PY'
import os, sys, time
base,n=sys.argv[1],int(sys.argv[2]); root=os.path.join(base,'metadata'); os.mkdir(root)
def timed(fn):
    t=time.perf_counter(); fn(); return time.perf_counter()-t
def create():
    for i in range(n):
        fd=os.open(os.path.join(root,f'f-{i:05d}'),os.O_CREAT|os.O_WRONLY|os.O_EXCL,0o600); os.close(fd)
def stats():
    for i in range(n): os.stat(os.path.join(root,f'f-{i:05d}'))
def rename():
    for i in range(n): os.rename(os.path.join(root,f'f-{i:05d}'),os.path.join(root,f'r-{i:05d}'))
def unlink():
    for i in range(n): os.unlink(os.path.join(root,f'r-{i:05d}'))
a=timed(create); b=timed(stats); c=timed(rename); d=timed(unlink); os.rmdir(root)
print(f"{n/a:.6f} {n/b:.6f} {n/c:.6f} {n/d:.6f}")
PY
)"
  read -r meta_create meta_stat meta_rename meta_unlink <<<"$meta_out"
  printf '[PERF] %s metadata ops/s create %.1f stat %.1f rename %.1f unlink %.1f\n' \
    "$n" "$meta_create" "$meta_stat" "$meta_rename" "$meta_unlink"

  printf '%s,%s,%s,"%s",%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$n" "$round" "$d" "${opts//\"/\"\"}" \
    "$write_s" "$write_rate" "$read_s" "$read_rate" "$random_s" "$random_iops" \
    "$fs_mean" "$fs_p50" "$fs_p95" "$fs_p99" "$fs_max" \
    "$meta_create" "$meta_stat" "$meta_rename" "$meta_unlink" >> "$CSV"

  sync
  sudo rm -rf -- "$dir"
  for i in "${!OUR_DIRS[@]}"; do [[ "${OUR_DIRS[$i]}" == "$dir" ]] && unset 'OUR_DIRS[i]'; done
}

section "RUN SAME-CARD BASELINE"
for ((r=1; r<=ROUNDS; r++)); do
  for n in "${NAMES[@]}"; do
    get_mount "$n"
    benchmark "$n" "$r" "$MOUNT_RESULT"
  done
done

section "SUMMARY"
python3 - "$CSV" <<'PY' | tee "$SUMMARY"
import csv, statistics, sys
rows=list(csv.DictReader(open(sys.argv[1],newline='')))
metrics=[
 ('seq_write_mib_s','Seq write','MiB/s',1),('seq_read_mib_s','Seq read','MiB/s',1),
 ('random_iops','Random 4K','IOPS',1),('fsync_mean_ms','fsync mean','ms',0),
 ('fsync_p95_ms','fsync p95','ms',0),('meta_create_ops_s','Create','ops/s',1),
 ('meta_stat_ops_s','Stat','ops/s',1),('meta_rename_ops_s','Rename','ops/s',1),
 ('meta_unlink_ops_s','Unlink','ops/s',1)]
names=[]
for r in rows:
    if r['filesystem'] not in names: names.append(r['filesystem'])
print('InfiltratorFS same-card performance baseline')
print('='*56)
for n in names:
    rr=[r for r in rows if r['filesystem']==n]
    print(f"\n{n} ({rr[0]['device']})")
    for k,l,u,_ in metrics:
        print(f"  {l:<14} {statistics.mean(float(x[k]) for x in rr):>12.3f} {u}")
if 'infiltratorfs' in names:
    inf=[r for r in rows if r['filesystem']=='infiltratorfs']
    print('\nInfiltratorFS relative to mature filesystems')
    print('-'*56)
    for other in ('ext4','xfs','btrfs'):
        oo=[r for r in rows if r['filesystem']==other]
        if not oo: continue
        print(f'\nvs {other}:')
        for k,l,_,higher in metrics:
            a=statistics.mean(float(x[k]) for x in inf)
            b=statistics.mean(float(x[k]) for x in oo)
            ratio=a/b
            suffix=f'{ratio*100:.1f}% throughput' if higher else f'{ratio:.3f}x latency'
            print(f'  {l:<14} {suffix}')
PY

printf '\nFull log: %s\nCSV: %s\nSummary: %s\n' "$LOG" "$CSV" "$SUMMARY"
printf 'PASS: all four filesystems completed the workload and integrity checks.\n'
