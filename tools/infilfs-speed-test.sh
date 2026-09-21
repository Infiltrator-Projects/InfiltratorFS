#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

mountpoint="${1:-}"
[[ -n "$mountpoint" && -d "$mountpoint" ]] || {
    echo "usage: sudo $0 <mounted-infiltratorfs-directory>" >&2
    exit 2
}
mountpoint="$(readlink -f "$mountpoint")"
fstype="$(findmnt -n -o FSTYPE -T "$mountpoint")"
[[ "$fstype" == infiltratorfs ]] || {
    echo "refusing non-InfiltratorFS target: $mountpoint ($fstype)" >&2
    exit 2
}

run_user="${SUDO_USER:-${USER:-root}}"
run_home="$(getent passwd "$run_user" | cut -d: -f6)"
[[ -n "$run_home" ]] || run_home="${HOME:-/tmp}"
stamp="$(date +%Y%m%d-%H%M%S)"
results="$run_home/infiltratorfs-speed-$stamp"
testdir="$mountpoint/.infilfs-speed-test-$stamp"
rand_source="/dev/shm/infilfs-speed-random-$stamp.bin"
mkdir -p "$results" "$testdir"
log="$results/results.txt"

cleanup() {
    sync -f "$testdir" 2>/dev/null || true
    rm -rf "$testdir" "$rand_source" 2>/dev/null || true
    group="$(id -gn "$run_user" 2>/dev/null || true)"
    [[ -n "$group" ]] && chown -R "$run_user:$group" "$results" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
exec > >(tee -a "$log") 2>&1

section() {
    printf '\n================================================================\n%s\n================================================================\n' "$*"
}
timed() {
    local name="$1"
    shift
    printf '\n---- %s ----\nSTART: %s\n' "$name" "$(date '+%Y-%m-%d %H:%M:%S.%N')"
    /usr/bin/time -f $'Elapsed: %E\nUser: %U sec\nSystem: %S sec\nCPU: %P\nMax RSS: %M KB\nMajor faults: %F\nMinor faults: %R\nVoluntary switches: %w\nInvoluntary switches: %c' "$@"
    printf 'END:   %s\n' "$(date '+%Y-%m-%d %H:%M:%S.%N')"
}
drop_caches() {
    sync -f "$testdir"
    echo 3 > /proc/sys/vm/drop_caches
}

section "INFILTRATORFS FORENSIC PERFORMANCE TEST"
echo "Started: $(date --iso-8601=ns)"
echo "Kernel:  $(uname -r)"
echo "Mount:   $mountpoint"
echo "Results: $results"
findmnt -T "$mountpoint" -o TARGET,SOURCE,FSTYPE,OPTIONS,SIZE,USED,AVAIL,USE%
df -hT "$mountpoint"
device="$(findmnt -n -o SOURCE -T "$mountpoint")"
dmesg --ctime | tail -n 250 > "$results/dmesg-before.txt" || true

section "RAW DEVICE READ BASELINE (READ-ONLY)"
drop_caches
timed "4 GiB raw O_DIRECT read"     dd if="$device" of=/dev/null bs=16M count=256 iflag=direct status=progress

section "SINGLE-PROCESS METADATA"
mkdir "$testdir/meta"
python3 - "$testdir/meta" <<'PY'
import os, sys, time
root=sys.argv[1]
n=10000
def run(name, fn):
    start=time.perf_counter()
    fn()
    print(f"{name}: {time.perf_counter()-start:.6f} s")
run("create 10000", lambda: [open(os.path.join(root,f"f-{i}"),"wb").close() for i in range(n)])
run("stat 10000", lambda: [os.stat(os.path.join(root,f"f-{i}")) for i in range(n)])
run("rename 10000", lambda: [os.rename(os.path.join(root,f"f-{i}"),os.path.join(root,f"r-{i}")) for i in range(n)])
run("delete 10000", lambda: [os.unlink(os.path.join(root,f"r-{i}")) for i in range(n)])
PY
timed "target sync after metadata" sync -f "$testdir"
rmdir "$testdir/meta"

section "INCOMPRESSIBLE SEQUENTIAL WRITE"
dd if=/dev/urandom of="$rand_source" bs=16M count=64 status=progress
drop_caches
timed "1 GiB buffered incompressible write"     dd if="$rand_source" of="$testdir/random.bin" bs=16M status=progress
timed "target sync after 1 GiB write" sync -f "$testdir"

section "COLD VERIFIED SEQUENTIAL READ"
drop_caches
timed "1 GiB cold filesystem read"     dd if="$testdir/random.bin" of=/dev/null bs=16M status=progress

section "O_DIRECT FILE I/O"
timed "1 GiB O_DIRECT write"     dd if="$rand_source" of="$testdir/direct.bin" bs=4M oflag=direct status=progress
drop_caches
timed "1 GiB O_DIRECT read"     dd if="$testdir/direct.bin" of=/dev/null bs=4M iflag=direct status=progress

section "FSYNC LATENCY"
python3 - "$testdir/fsync.bin" <<'PY'
import os, statistics, sys, time
path=sys.argv[1]
values=[]
fd=os.open(path,os.O_CREAT|os.O_TRUNC|os.O_WRONLY,0o600)
try:
    block=b'Z'*4096
    for _ in range(1000):
        start=time.perf_counter_ns()
        os.write(fd,block)
        os.fsync(fd)
        values.append((time.perf_counter_ns()-start)/1e6)
finally:
    os.close(fd)
values.sort()
pct=lambda p: values[int((len(values)-1)*p)]
print(f"min={values[0]:.3f} ms avg={statistics.mean(values):.3f} ms median={statistics.median(values):.3f} ms p95={pct(.95):.3f} ms p99={pct(.99):.3f} ms max={values[-1]:.3f} ms")
PY

section "CREATE + FSYNC LATENCY"
python3 - "$testdir" <<'PY'
import os, statistics, sys, time
root=sys.argv[1]
values=[]
for i in range(500):
    path=os.path.join(root,f"sync-{i}")
    start=time.perf_counter_ns()
    fd=os.open(path,os.O_CREAT|os.O_TRUNC|os.O_WRONLY,0o600)
    os.write(fd,b'x')
    os.fsync(fd)
    os.close(fd)
    values.append((time.perf_counter_ns()-start)/1e6)
values.sort()
pct=lambda p: values[int((len(values)-1)*p)]
print(f"min={values[0]:.3f} ms avg={statistics.mean(values):.3f} ms median={statistics.median(values):.3f} ms p95={pct(.95):.3f} ms p99={pct(.99):.3f} ms max={values[-1]:.3f} ms")
for i in range(500):
    os.unlink(os.path.join(root,f"sync-{i}"))
PY
timed "target sync after delete" sync -f "$testdir"

if command -v fio >/dev/null 2>&1; then
    section "OPTIONAL FIO CROSS-CHECK"
    fio --name=randread --filename="$testdir/fio.bin" --size=1G --rw=randread         --bs=4k --iodepth=32 --direct=1 --runtime=30 --time_based         --group_reporting --output="$results/fio-randread.txt" || true
    fio --name=randwrite --filename="$testdir/fio.bin" --size=1G --rw=randwrite         --bs=4k --iodepth=32 --direct=1 --runtime=30 --time_based         --group_reporting --output="$results/fio-randwrite.txt" || true
else
    echo "fio not installed: optional fio cross-check skipped; core benchmark is complete."
fi

section "KERNEL LOG DELTA"
dmesg --ctime | tail -n 500 > "$results/dmesg-after.txt" || true
diff -u "$results/dmesg-before.txt" "$results/dmesg-after.txt"     > "$results/dmesg-difference.txt" || true

section "COMPLETE"
echo "Finished: $(date --iso-8601=ns)"
echo "Main log: $log"
