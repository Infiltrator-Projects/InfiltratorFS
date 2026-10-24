<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# PERF-019 — Excessive native Linux final synchronization latency

**Status:** reproducible final-sync latency on operator-tested Format 0.19
loop-backed media; stage-level bottleneck attribution and physical-NVMe
qualification remain **open**. Format 0.19 functional mounted qualification is
recorded separately in `QUALIFICATION.md`. Neither clean fsck/scrub nor
loop-backed timings establish production durability or physical-media speed.

**Provenance:** operator-supplied real-machine engineering handoff, received
18 October 2026. Numbers below are reproduced from that handoff; they have
not been rerun as physical-media Format 0.19 measurements. The observations are not
normalised for differing cache state. Performance conclusions must not be
silently promoted to Format 0.19 qualification.

## Hardware and software

Dell Latitude 5550, Linux kernel `7.0.0-38-generic`.
Destination ~2 TB KXG80 NVMe `/dev/nvme0n1` with EXT4 on
`/dev/nvme0n1p2` (800 GB, system `/`) and InfiltratorFS on
`/dev/nvme0n1p5` (~1 TB, `/mnt/infiltratorfs`). Mount options:
`rw,relatime,media=nonrotational,compress=auto`.
Source was a separate 256 GB NVMe `/dev/nvme1n1`, NTFS partition p2
mounted read-only via NTFS-3G at `/mnt/windows-source`.

InfiltratorFS was initially formatted with software 0.18.110 and upgraded
to software 0.18.114 without reformatting, retaining **disk Format 0.18**.
A structural fsck before the 0.18.114 benchmark was reported CLEAN at
generation 1016, with 3/3 checkpoints valid.

## Reproducible workload observations

Deterministic seed `18110`: 1,460 genuine Windows files, 589.7 MiB,
selected from System32, SysWOW64, Fonts and INF. Symlinks excluded.
Copied to both filesystems, with destination order reversed in the second
round. Timed buffered copy, `sync -f` and independent SHA-256 validation
of every copied file; all reported integrity comparisons passed.

| Software | Filesystem | Round | Buffered copy (s) | Final sync (s) | Total (s) |
|---|---|---:|---:|---:|---:|
| 0.18.110 | EXT4 | 1 | 2.221 | 0.210 | 2.431 |
| 0.18.110 | InfiltratorFS | 1 | 1.653 | 11.128 | 12.781 |
| 0.18.110 | InfiltratorFS | 2 | 1.730 | 10.328 | 12.058 |
| 0.18.110 | EXT4 | 2 | 0.972 | 0.132 | 1.104 |
| 0.18.114 | EXT4 | 1 | 0.633 | 0.190 | 0.823 |
| 0.18.114 | InfiltratorFS | 1 | 1.118 | 5.089 | 6.207 |
| 0.18.114 | InfiltratorFS | 2 | 1.102 | 5.175 | 6.277 |
| 0.18.114 | EXT4 | 2 | 0.634 | 0.137 | 0.771 |

**Observed:** 0.18.114 completed buffered copies in ~1.1 s but took
~5.1 s for the final sync, versus ~0.16 s for EXT4. The apparent
improvement from 0.18.110 is not causally attributable to a particular
commit because cache state and other machine conditions varied.

Single-file test: repeated writes of a 1 MiB random-data buffer
(total 256 MiB), followed by `sync -f`.

| Software | Filesystem | Buffered write (s) | Final sync (s) | Total (s) |
|---|---|---:|---:|---:|
| 0.18.110 | EXT4 | 0.095 | 0.129 | 0.224 |
| 0.18.110 | InfiltratorFS | 0.134 | 1.001 | 1.136 |
| 0.18.114 | EXT4 | 0.104 | 0.100 | 0.204 |
| 0.18.114 | InfiltratorFS | 0.116 | 0.997 | 1.113 |

A one-second final sync persists even for a single large file. Two
200-operation 4 KiB write+`fsync` runs on 0.18.110 gave average
InfiltratorFS 1.513 ms (compressible) and 1.467 ms (random),
versus EXT4 0.744 ms and 0.762 ms. Maximum observed InfiltratorFS
latency was 12.593 ms. Similar latency on compressible and random
payloads does **not** suggest compression is the primary `fsync`
bottleneck in those samples. The native warning threshold of 100 ms
misses cumulative sequences of small commits. Idle `sync -f` was
below reported 0.01-second timing resolution.

These are limited operator measurements, not a statistically complete
comparison or direct proof of the bottleneck stage. Buffered-copy speed
and SHA-256 matching do not establish power-failure durability.

## 22 October 2026 — Format 0.19 mounted real-file and trace evidence

**Provenance:** operator-supplied terminal output from Dell Latitude 5550,
Linux `7.0.0-38-generic`, installed InfiltratorFS software `0.19.31`,
newly formatted disk Format 0.19. The test target was a disposable 2 GiB
loop-backed file **on the host's EXT4 root volume**, not the historical
1 TB Format 0.18 NVMe partition. The original on-disk 0.18 partition
remained unchanged. Source documents were staged from NFS
(`192.168.1.240:/srv/nfs4/Share/Shared_Documents`) into the user's home
directory, so the timed runs used locally staged source files.

The initial Format 0.19 real-document test selected 329 files, 536,870,880
bytes (512 MiB), before reaching its byte limit. Buffered copy took 665 ms;
the subsequent `sync -f` took 2,725 ms. Both byte-by-byte comparisons
(before and after remount) passed; structural `fsck -n` returned CLEAN and
read-only deep scrub checked 329 files and 131,237 data blocks with zero
checksum and metadata errors.

A separate two-image reference run using the same staged source tree yielded:

| Loop-backed target | Buffered copy (ms) | Final `sync -f` (ms) | Sum (ms) |
|---|---:|---:|---:|
| EXT4 reference | 392 | 237 | 629 |
| InfiltratorFS 0.19 | 649 | 2,809 | 3,458 |

Both image contents matched the source after a read-only remount;
InfiltratorFS's structural check was CLEAN. These are **single
observations**, not isolated physical-media measurements. A filesystem in
a host file shares cache, allocation and durability effects with its host;
the comparison is a latency signal, not a claim that EXT4 owns any
InfiltratorFS semantics or is a performance specification.

A third 0.19 disposable-image run produced a buffered copy of 1,020 ms and
a final `sync -f` of 3,018 ms, with clean structural check and identical
content. During the final sync, an isolated kernel `function_graph` trace
covered five exposed symbols. An operator-side parser reported:

| Traced function | Completed calls | Sum of inclusive durations (ms) | Longest call (ms) |
|---|---:|---:|---:|
| `infilfs_writeback_cluster_submit` | 378 | 2,721.76 | 38.30 |
| `infilfs_writepages` | 288 | 2,582.19 | 48.86 |
| `infilfs_native_pending_commit_locked` | 7 | 132.54 | 24.47 |
| `infilfs_sync_fs` | 2 | 20.45 | 20.45 |

**Interpretation boundary:** function-graph totals are *inclusive*, can
nest and overlap across CPUs, and are not a decomposition of syscall wall
time. The trace supports prioritising writeback/CoW preparation over the
final `sync_fs` entry point for further attribution, but does not prove
that compression, hashing, folio staging, allocation or device I/O is
individually excessive. Two internal static functions
(`infilfs_rw_tx_commit` and `infilfs_rw_write_checkpoint_replicas_sync`)
were not separately exposed to the tracer; both are present in source,
so that tracing limitation does **not** indicate missing filesystem
functionality.

**Engineering next step:** attribute each writeback cluster's staging,
digest/codec preparation, reservation/CoW work, lock waits, staged I/O
submission and completion; confirm the same workload on an explicitly
approved *disposable* raw target before changing cache/writeback or durable
checkpoint ordering. Do not remove the dirty-folio snapshot, transaction
publication barriers or recovery requirements solely to reduce this
benchmark time. No performance fix is claimed by recording these results.

## Format 0.19.33 source optimisation candidate (22 October 2026)

The native page-cache cluster submitter already stages an immutable contiguous
snapshot and keeps it alive throughout `infilfs_native_writeback_iter()`.
Previously, the multi-cluster parallel-append helper allocated another buffer
and copied the entire staged writeback window before queuing its workers.
For this **specific page-cache-owned single-kvec path only**, the helper now
borrows the existing snapshot and skips the redundant allocation/copy.
All preparation completions are joined before returning to the owner;
generic/direct/user iterators continue to use an owned snapshot. Iterator
advance/revert and partial-commit rules are unchanged. No disk-format or
durability-ordering changes are made.

This removes real redundant work but **is not yet a measured PERF-019
resolution**. Test the same 512 MiB real-file source tree, compare copy and
final-sync separately across repeated runs, remount and deep scrub before
claiming a speedup. Investigate remaining writeback work if the reduction
does not materially change the three-second loop-backed latency.

## 22 October 2026 — 0.19.33 operator qualification and next source change

On the same Dell laptop, the operator compiled the exact
`e86edab1b0424093f4eb7a29464c11ff4941ceab` 0.19.33 kernel
module for `7.0.0-38-generic`, loaded it temporarily and mounted a new
disposable 2 GiB Format 0.19 image. The installed 0.19.31 formatter
created the volume; on-disk Format 0.19 was unchanged. The same 329-file,
512 MiB staged real-data set was copied and verified after a read-only
remount. Structural check and deep scrub were CLEAN (131,237 data blocks,
zero data checksum errors, zero metadata errors).

| Module tested | Copy (ms) | Final sync (ms) | Total (ms) |
|---|---:|---:|---:|
| Installed 0.19.31 reference | 649 | 2,809 | 3,458 |
| Temporary 0.19.33 | 1,031 | 2,617 | 3,649 |

These are different single-run observations; do **not** interpret the
192 ms lower sync time as demonstrated improvement, especially as total time
was higher. No performance release gate is closed.

Source inspection found the one-copy optimisation from 0.19.33 covers
only *multi-cluster* parallel append. The ordinary sequential append path
still copied each already-staged immutable page-cache kvec into a second
temporary buffer before checksum/compression preparation. The next candidate
reuses the same bounded borrow helper for both paths, preserving iterator
advance/rollback and owned copies for non-page-cache writers. This is a
source-level reduction of redundant work, **not a proven latency fix**.

## Instrumentation plan — measure before tuning

Use opt-in tracepoints or bounded counters, preferably reporting counts,
cumulative microseconds, maxima, plus distribution buckets for p50/p95/p99.
Avoid log spam and per-folio dynamic allocations on hot paths. Time with
`ktime_get_ns()` or equivalent monotonic high-resolution clock and convert
only for presentation. Attribution must cover the complete syscall's
writeback and persistence, not merely the final checkpoint writer:

1. `kernel/infiltratorfs_pagecache.c`:
   `infilfs_writepages()` and `infilfs_writeback_cluster_submit()`;
   folio collection and lock wait, staging, native writeback and completion.
2. `kernel/infiltratorfs_rw_data.inc`:
   `infilfs_native_pending_commit_locked()`,
   `infilfs_native_pending_flush_sb()` and `infilfs_sync_fs()`;
   pending count/bytes, lock-wait latency, work queued before sync,
   deferred publication counts and total filesystem sync duration.
3. `kernel/infiltratorfs_rw_legacy.inc`:
   `infilfs_rw_tx_commit()`,
   `infilfs_rw_write_checkpoint_replicas_sync()`;
   allocation tree, metadata, dependency submissions/completions,
   dependency flush, primary write and *durable* primary flush,
   secondary writes and their final flush. Existing 100 ms warning
   is not sufficient for cumulative work attribution.
4. Attribute compression/checksum work and CoW extent allocation
   separately where safe. Record lock contention and wait-queue delay
   distinctly from active CPU/IO time. Account for background writeback
   that occurs before entry into `sync_fs`; ideally pair trace events with
   `sync -f` userspace wall time and kernel writeback trace events.

**Do not infer** flushes are costly merely because multiple flush calls exist.
Do not change ordering or eliminate required barriers without crash-safety
proof. In particular, preserve a discoverable complete generation while
replacing checkpoints. Relocation retains the old primary geometry map until
new remote checkpoints are independently durable, as specified in
`FORMAT-0.19-RECOVERY-DESIGN.md`.

## Format 0.19 required invariants

- Three separated authoritative checkpoints remain, with complete graph
  validation before recovery selects a generation.
- Both fixed discovery locators at blocks 1 and 2047 are **hints**, not
  transaction commit records. If checkpoint offsets do **not** move, ordinary
  writes, transaction commits and `fsync` **must not write locators**.
- Geometry-changing operations publish locators under a separately
  fault-injected crash-consistent protocol; do not shift that extra
  write amplification into ordinary file I/O.
- Avoid full allocation-tree rewrites where incremental safe publication
  suffices, but never weaken CoW or checksum validation.
- Maintain one portable disk contract across Linux and Windows.

## Physical benchmark/recovery acceptance matrix (not yet passed)

**A. Large sequential write:** 256 MiB and larger, separately time
buffered copy/write, sync, total, per-file SHA-256 and post-remount read.

**B. Mixed real-file tree:** repeat the same seed `18110`, 1,460-file,
589.7 MiB source selection where available. Reverse destination order
and repeat; track cache state and filesystem geometry/free space.

**C. Small durable writes:** 4 KiB durable writes with median, p95,
p99, maximum, plus count, distribution and full syscall latency.

**D. Metadata:** thousands of create/rename/unlink operations with
separate write and sync timing, plus tree integrity.

**E. Persistence:** following completed sync, unmount/remount,
compare content hashes and run fsck/deep scrub. Crash/fault testing
must use *disposable* images or explicitly approved dedicated test
media, injecting interruption at each dependency, checkpoint,
locator and flush boundary. Report whether the newest complete
committed generation remains recoverable, or the last safely
committed previous generation where interruption occurred.

Every result records the exact Git SHA, software and on-disk format,
kernel, source/destination device and filesystem types, mount
options, cache controls, trial ordering, capacity/free-space,
CPU, storage model, and sync technique. Record phase times and
errors—not only headline MB/s.

**Safety:** Never automatically format the current EXT4 root,
`/dev/nvme0n1p2`, the attached InfiltratorFS development partition
`/dev/nvme0n1p5`, or an assumed `/dev/mmcblk0` layout. No live
partition is modified as a result of this document.

**Gate:** PERF-019 cannot close merely because Format 0.19
formats, mounts or passes normal CI. It closes only with
independent profiling and reproducible physical-media evidence
of acceptable efficiency **without** loss of crash correctness.


## 23 October 2026 — bounded physical-NVMe evidence entrypoint

Run `python3 tests/perf-019-qualification.py run --mount /mnt/infiltratorfs-test --report "$HOME/perf019-report.json"` to perform a **read-only preflight**.
The collector accepts only the root of an already mounted, read-write, native
InfiltratorFS filesystem directly backed by an NVMe block device; it will never
format, mount, unmount, power-cycle, offline-scrub or delete data.

Only after checking the exact selected non-system *disposable* Format 0.19
volume, free space, source and loaded driver, add `--execute` to perform
three bounded physical-media rounds. Optional `--tree /path/to/staged-source`
measures a real external source tree and records whether it contains 1,460
regular files. No sample source tree is invented.

After a **genuine unmount/remount** of that same volume, run
`python3 tests/perf-019-qualification.py verify --mount /mnt/infiltratorfs-test --report "$HOME/perf019-report.json"`. Verification
rejects a still-live mount instance and compares every retained byte count
and SHA-256. Offline `fsck.infiltratorfs --scrub` is a separate operator step
with the volume unmounted. The tool never removes retained data automatically.

This is an evidence **collection facility**, not current physical-NVMe data.
It does not measure kernel-stage CoW/lock/writeback attribution and cannot
qualify forced physical power interruption or native Windows mounts. Those
gates remain unchecked until actually measured and evidenced.

## Opt-in native write-path attribution (implementation, not a speedup claim)

The native Linux module has a disabled-by-default diagnostic switch
`perf019`. The switch is a module parameter, not a mount option.
After testing a newly built module, an operator can load it with
`perf019=1`, or enable capture on that installed version using
`sudo sh -c 'echo 1 > /sys/module/infiltratorfs/parameters/perf019'`.
**Do not replace, unload, or reinstall an active mounted driver.**
The published 0.19.36 binary does not contain these diagnostics.

Per-mount atomic counters sample page-cache writeback staging, synchronous
native CoW writeback, allocation-map publication, dependency I/O,
dependency durability flushing and ordered checkpoint publication.
Unmodified file-write and sync callbacks remain covered by external wall-time
measurements. A clean unmount emits each populated stage with `calls`,
`total_us`, `avg_us` and `max_us`:

```
journalctl -k -b --no-pager | grep 'InfiltratorFS PERF019'
```

The samples are inclusive and overlapping. Concurrent workers make sum-of-phase
CPU time different from wall time; these counters do not reveal SHA-256,
compression, individual 4 KiB buffer-head overhead, or writer-lock wait
in isolation. Capturing those stages requires a separate maintainable
implementation, not unbounded additions to the already-dense native data
translation unit. Disabled capture performs neither timestamps nor atomic
updates; an in-flight sample is completed after capture is turned off. Counters
reset with the next mount, not the parameter toggle. The module introduces no
new persistent state, I/O, error path or durability reordering.

This is instrumentation only, not a measured performance improvement.
The acceptance gate remains repeatable physical-NVMe timing plus post-remount
hashes, structural checking, deep scrub and crash-recovery qualification.

## 24 October 2026 — fragmented-reservation search improvement

The native sharded allocator previously tested adjacent candidate starts one
block at a time. When an unavailable block lay near the end of a proposed
reservation, that block was reconsidered by numerous subsequent overlapping
candidates. The search now returns the first unavailable block and skips every
start through that blocker. Both the authoritative on-disk allocation bitmap
and volatile shard-lock/journal semantics are unchanged. The deterministic
reference-model regression exhaustively compares old and new two-pass search
results for all occupancy patterns through eleven blocks. CI still must
compile and qualify the actual kernel implementation.

This is a targeted improvement for **fragmented allocation search**, not
evidence that the physical-NVMe 492 MiB/s sequential sync bottleneck has been
resolved. That claim still requires exact-version physical measurements.
