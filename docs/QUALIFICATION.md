<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Qualification Record

This file records significant qualification runs against specific source commits. It is evidence for implemented behavior, not a claim that InfiltratorFS is bug-free.

## 2026-08-30 scale, endurance and online-defragmentation qualification

The next three Linux roadmap items were qualified at exact source commit
`a40a9a9a8f12b4789c3e582b92abf5678a07e79e` (Format 0.17). Native Linux workflow
run `33278009197` completed successfully and established:

- one million mounted regular files across bounded directory fan-out, followed
  by 100,000 unlink/recreate operations, durable read-only remount verification
  and a CLEAN offline scrub;
- a separate 1 TiB sparse loop-backed volume with non-zero extent I/O, thousands
  of namespace objects, a 900 GiB sparse high-offset file, read-only remount
  verification and a CLEAN offline scrub;
- a bounded near-full 4 GiB workload with fragmentation/refill size classes,
  hole-punch/refill, five minutes of concurrent mixed data and metadata I/O,
  durable-manifest verification across read-only remount and two CLEAN scrubs;
  and
- native per-file fragmentation metrics plus bounded copy-on-write online
  defragmentation preserving file content, inode/hard-link identity, xattrs,
  timestamps and retained-snapshot data across CLEAN scrub and remount.

The ordinary portable jobs on the same commit passed GCC, Clang, sanitizer,
static-analyzer, Windows and native-kernel qualification. Its native-package job
exposed a malformed optimizer manifest edit; the completion change restored the
canonical package script, retained `infilfs-optimize` and its kernel ABI sources
in both native package formats, added package-syntax and complete-harness policy
guards, and passed:

- a strict GCC warnings-as-errors build and all 30 CTest tests;
- every scale, endurance and online-defragmentation policy guard;
- bounded userspace scale and mixed-workload generator smoke tests; and
- `.deb` plus self-extracting `.run` construction, manifest checks, installer
  verification and SHA-256 verification.

`tests/native-complete-qualification.sh` composes the complete installed-release,
physical partition-22, portable, package, kernel, online-defrag, million-file,
1 TiB and near-full endurance suites into one explicitly destructive operator run.
It also enforces conservative regression floors against the 2026-08-29 physical
partition baseline.

## 2026-08-29 full checked-roadmap audit

The 56 roadmap entries marked complete at source commit
`075aed9c737fb38cc408d752736a97773dc2a035` (release 0.18.24,
Format 0.17) were requalified across portable tests, exact-current-main CI,
the native Linux kernel path and destructive physical-media testing.

### Portable and CI evidence

- GCC: complete 30-test CTest suite passed.
- Clang: complete 30-test CTest suite passed.
- All 10 in-tree format/native policy guards passed.
- Explicit current-format tree, crash/recovery, integrity, sparse-file,
  forensic, system-utilities, desktop-integration and Manager regressions passed.
- Exact-current-main Build and conformance workflow run `33248565388` passed,
  including Linux, Clang, sanitizer/analyzer, package and Windows-native jobs.
- Exact-current-main Native Linux kernel module workflow run `33248565389`
  passed.
- The published 0.18.24 release gate passed for the qualified release commit,
  including native mounted write/read, sync, unmount and CLEAN scrub.

### Physical native-VFS evidence

The destructive qualification target was `/dev/mmcblk0p22`, an
89,678,413,824-byte test partition, on Linux kernel `7.0.0-30-generic`.

The maintained partition-22 native VFS qualification completed with:

- 69 passed, 0 failed, 0 warnings;
- native kernel VFS only, with no FUSE mount;
- Format 0.17 create/read/write/remount coverage;
- inline and extent-backed data verification;
- sparse growth, `fallocate`, hole punching and allocation reporting;
- 1023-byte filename acceptance and 1024-byte rejection;
- hard links, symbolic links, rename/unlink/rmdir and open-handle semantics;
- persistent Linux xattrs and special-node identity;
- writable shared `mmap` writeback;
- retained-snapshot verification;
- 4,000 mounted random 4 KiB overwrites;
- 200 explicit fsync publications;
- read-only remount persistence checks; and
- post-workload and final offline scrubs reporting CLEAN.

Four additional physical concurrency rounds then passed. Each round exercised
six namespace mutators, 144 files, six shared-inode writers, three xattr
writer/reader groups and six open-unlink writers. The round durations were
66.352 s, 76.741 s, 78.914 s and 84.220 s.

After those rounds, a fresh read-only remount and complete root enumeration
passed. The final offline scrub reported:

- generation: 3695;
- files checked: 1,446;
- data blocks checked: 151,580;
- snapshots checked: 1;
- checksum errors: 0;
- metadata errors: 0;
- result: CLEAN;
- scrub duration: 18.913 s.

A corrected kernel-log check over the qualification interval found no
`EUCLEAN`, "Structure needs cleaning", kernel `BUG:`, Oops, panic, hung-task,
soft-lockup or hard-lockup signatures. The administrator emergency module-load
override remained active after qualification as `install /bin/false`.

### Observed physical-media performance

These numbers describe this single physical test device and workload; they are
qualification telemetry, not cross-filesystem benchmark claims.

- Format 83.5 GiB partition: 0.773 s.
- Native read-write mount: 0.722 s.
- 512 MiB sequential write: 24.35 MiB/s.
- Verified sequential read: 127.84 MiB/s.
- 4 KiB random overwrite workload: 109.46 IOPS over 4,000 writes.
- fsync latency over 200 explicit publications:
  mean 30.658 ms, p50 24.639 ms, p95 79.236 ms, p99 156.146 ms,
  maximum 724.354 ms.
- Post-write offline scrub: 14.910 s.
- Final maintained-harness offline scrub: 14.822 s.
- Final post-concurrency offline scrub: 18.913 s.

### Audit-harness corrections

Three audit-wrapper false negatives were identified and corrected during the
run; none represented an InfiltratorFS failure:

1. The mount helper was initially checked with a literal source grep for
   `mount -i`. The helper correctly constructs
   `args=(-i -t infiltratorfs)`, and the argument-level
   `tests/system-utilities.sh` regression passed.
2. Post-concurrency enumeration was initially run as the desktop user against
   root-owned mode-0700 qualification directories. Re-running the read-only
   enumeration as root passed.
3. A case-insensitive diagnostic grep interpreted an unrelated firmware message
   containing "Firmware bug:" as kernel `BUG:`. A corrected case-sensitive
   failure-signature check found no corruption/lockup-class kernel diagnostics.

The qualification therefore supports the 56 checked roadmap claims at the
exact audited source commit. It does not establish the unchecked roadmap items,
nor does it prove absence of defects outside the exercised contracts.
