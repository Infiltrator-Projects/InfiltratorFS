<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# PERF-019 — Excessive native Linux final synchronization latency

**Status:** confirmed historical measurements; bottleneck attribution and
Format 0.19 regression qualification are **open**. This is a performance
investigation. Format 0.19 functional mounted qualification is recorded separately in `QUALIFICATION.md`; these historical timings do not establish its performance.

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
proof. In particular, the primary new checkpoint must be independently
durable before secondary replicas are replaced.

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
