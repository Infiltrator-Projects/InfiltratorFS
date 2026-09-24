#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail

build="${1:-build}"
module="${2:-kernel/infiltratorfs.ko}"
image="$(mktemp --suffix=.img)"
mountpoint="$(mktemp -d)"
loopdev=""
mounted=0
loaded=0
start_time="$(date '+%Y-%m-%d %H:%M:%S')"

cleanup() {
    set +e
    if [[ "$mounted" = 1 ]]; then sudo umount "$mountpoint" 2>/dev/null || true; fi
    if [[ -n "$loopdev" ]]; then sudo losetup -d "$loopdev" 2>/dev/null || true; fi
    if [[ "$loaded" = 1 ]]; then sudo rmmod infiltratorfs 2>/dev/null || true; fi
    rm -f "$image"
    rmdir "$mountpoint" 2>/dev/null || true
}
trap cleanup EXIT

# Start from a fresh module instance so the CPU-pool and per-mount peaks belong
# only to this qualification workload.
if lsmod | grep -q '^infiltratorfs '; then
    sudo rmmod infiltratorfs
fi
sudo insmod "$module"
loaded=1

pool_line="$(sudo dmesg --since "$start_time" 2>/dev/null |
    grep 'InfiltratorFS: CPU pool online_logical_cpus=' | tail -n1)"
cores="$(sed -nE 's/.*online_physical_cores=([0-9]+).*/\1/p' <<<"$pool_line")"
budget="$(sed -nE 's/.*filesystem_budget=([0-9]+).*/\1/p' <<<"$pool_line")"
[[ "$cores" =~ ^[0-9]+$ && "$cores" -ge 1 &&
   "$budget" =~ ^[0-9]+$ && "$budget" -ge 1 ]] || {
    echo "native CPU parallelism: could not resolve physical-core budget" >&2
    echo "$pool_line" >&2
    exit 1
}
expected_budget=$(( cores > 1 ? cores - 1 : 1 ))
(( budget == expected_budget )) || {
    echo "native CPU parallelism: budget=$budget expected=$expected_budget for cores=$cores" >&2
    echo "$pool_line" >&2
    exit 1
}
# A one-slot environment can verify the physical-core N-1 cap but cannot
# prove that independent work scales concurrently. Treat that topology as an
# explicit qualification skip rather than a product failure; multi-worker proof
# remains mandatory whenever the runner exposes at least two filesystem slots.
if (( budget < 2 )); then
    printf 'Native N-1 physical-core parallelism qualification skipped: cores=%s budget=%s (multi-worker proof requires budget >= 2)\n' \
        "$cores" "$budget"
    exit 0
fi

truncate -s 512M "$image"
"$build/mkfs.infilfs" -L cpu-parallel "$image" >/dev/null
loopdev="$(sudo losetup --find --show "$image")"
sudo mount -t infiltratorfs -o rw "$loopdev" "$mountpoint"
mounted=1

# A long contiguous dirty stream is submitted through normal buffered writeback.
# The dynamic batch contains at least one 256 KiB compression cluster per CPU
# budget slot, so the real digest/compression/reservation workers—not a synthetic
# benchmark—must be able to overlap up to N-1.
sudo python3 - "$mountpoint/parallel.bin" <<'PY'
import os
import sys

path = sys.argv[1]
pattern = bytes((i * 73 + 19) & 0xff for i in range(1024 * 1024))
with open(path, "wb", buffering=0) as stream:
    for _ in range(128):
        stream.write(pattern)
    os.fsync(stream.fileno())
PY
sudo sync
sudo umount "$mountpoint"
mounted=0
sudo losetup -d "$loopdev"
loopdev=""

allocator_line="$(sudo dmesg --since "$start_time" 2>/dev/null |
    grep 'InfiltratorFS: allocator reservations=' | tail -n1)"
peak="$(sed -nE 's/.*prepared_append_peak_active=([0-9]+).*/\1/p' <<<"$allocator_line")"
attempts="$(sed -nE 's/.*prepared_append_attempts=([0-9]+).*/\1/p' <<<"$allocator_line")"
successes="$(sed -nE 's/.*prepared_append_successes=([0-9]+).*/\1/p' <<<"$allocator_line")"
[[ "$peak" =~ ^[0-9]+$ && "$attempts" =~ ^[0-9]+$ &&
   "$successes" =~ ^[0-9]+$ ]] || {
    echo "native CPU parallelism: missing mounted preparation telemetry" >&2
    echo "$allocator_line" >&2
    exit 1
}
(( attempts >= budget )) || {
    echo "native CPU parallelism: only $attempts preparation attempts for budget $budget" >&2
    exit 1
}
(( successes > 0 )) || {
    echo "native CPU parallelism: no prepared append completed" >&2
    exit 1
}
(( peak == budget )) || {
    echo "native CPU parallelism: real prepared-write peak $peak did not fill budget $budget" >&2
    echo "$allocator_line" >&2
    exit 1
}

"$build/fsck.infiltratorfs" --scrub "$image" |
    grep -Fq 'Result:              CLEAN'
printf 'Native N-1 physical-core parallelism qualification passed: cores=%s budget=%s peak=%s attempts=%s successes=%s\n' \
    "$cores" "$budget" "$peak" "$attempts" "$successes"
