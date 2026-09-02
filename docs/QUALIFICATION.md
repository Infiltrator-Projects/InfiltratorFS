<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Qualification Record

This file records significant qualification runs against specific source commits. It is evidence for implemented behavior, not a claim that InfiltratorFS is bug-free.

## Qualification cadence

InfiltratorFS deliberately separates fast regression from expensive qualification.

- **Build and conformance** remains the broad every-main-push gate. Superseded runs
  are cancelled. GCC/Linux, Clang, sanitizers, static analysis, Windows
  portability and package construction remain ordinary regression coverage.
- **Local Linux userspace** proves a native build and cross-platform image on the
  real local runner environment without repeating the complete GCC CTest suite.
- **GNOME Disks formatter integration** builds the pinned libblockdev/UDisks/GNOME
  Disks stack only when the formatter/integration surface changes, plus the
  scheduled weekly and explicit manual comprehensive runs.
- **Native Linux kernel module** remains the ordinary mounted kernel qualification
  for kernel/core/package changes and every explicit Release commit; unrelated
  documentation pushes do not spend kernel-runner time.
- **Heavy filesystem qualification** owns the million-file/1 TiB and near-full
  mixed-workload endurance suites. It runs for storage/core changes, weekly, or
  manually. Superseded heavy runs are cancelled rather than finishing obsolete
  source revisions.
- **Physical partition qualification** remains an explicitly destructive operator
  milestone/audit run and is never unattended ordinary CI.
- **Release publication** still rebuilds, installs, mounts and cross-checks the
  exact release source and artefacts before immutable publication.

The purpose is to preserve the hard-earned regression coverage while matching
test cost to the code that can actually invalidate each qualification.

## 2026-09-01 Windows bridge performance and manager qualification

Development commit `ba1b06561de13dfa434b5cebf2e5797022d83572`
passed Build and conformance workflow run `33474719972`. The Windows job
`99751681139` built the 0.18.33 native manager and portable core with MSVC,
passed all 19 Windows/portable CTest tests, opened the Linux-created Format
0.17 interoperability image and completed the real hosted ProjFS provider
qualification.

The Windows bridge qualification now includes an 8 MiB existing file followed
by a 4096-byte in-place Windows edit at the 4 MiB offset. The bridge records
write-back statistics and the test requires less than 1 MiB of InfiltratorFS
write-back for that edit. This prevents regression to the former close-time
whole-file rewrite behaviour. The same ProjFS run also requalified
Linux-created-file hydration/editing, Windows create/write, directory rename,
hard-link identity and delete persistence.

The 0.18.33 bridge coalesces Explorer mutation bursts and publishes after a
short idle interval rather than forcing a checkpoint for every created or
closed file; stopping the bridge still forces final durability. The Manager's
direct-copy path also removes the redundant pre-rename publication so the
staged data and namespace rename can be committed together.

The Windows executable was also rebuilt with explicit UTF-8 MSVC source
handling, Unicode Win32 APIs, Common Controls v6 visual styles, per-monitor DPI
awareness, light/dark palette handling and file-type icons. This addresses the
mojibake and legacy-control appearance seen in 0.18.32 without changing Format
0.17.

The same development source passed Native Linux kernel workflow run
`33474719970`, including the complete mounted read-write transaction/scrub
qualification, media-aware placement and online defragmentation. Linux full
suite, Clang, ASan/UBSan, GCC static analysis and native package construction
also passed in run `33474719972`.

## 2026-08-31 desktop formatter and parallel-allocation qualification

The last two Linux roadmap items selected for release 0.18.28 were qualified
against exact `main` source.

Exact source commit `b9700642e8eb70c7969124a7add21429608f5003`
passed Build and conformance workflow run `33380655074`, including GNOME Disks
formatter job `99452037637`. The job applied the repository's version-pinned
patches to libblockdev `7ce6c6e9b59fdf16cd5dffd602f954ef52588dbb`, UDisks
`c731cee133bb3240a0b91b59f9995a42aafb0ac4` and GNOME Disks
`dc2da843a7afc7f9e916999d5daeecb0d6adf84c`, then:

- built and installed the patched libblockdev filesystem plugin;
- required InfiltratorFS `MKFS`, label and force capability discovery;
- invoked libblockdev's generic `fs_mkfs()` path against a 64 MiB image with
  the label `GNOME Disks Integration`;
- verified Format 0.17 type and label output through
  `infilfs-inspect --udev`;
- built the patched UDisks/libudisks2 source; and
- completed all 142 targets in the patched GNOME Disks Meson build.

The same exact source commit's Native Linux workflow run `33380655054`, kernel
qualification job `99452036592`, built both the generic and running-kernel
modules, reproduced the self-contained DKMS build and passed the complete
mounted native transaction suite. Its explicit parallel gate
started 16 writers together, wrote and hash-verified 16 disjoint 4 MiB files,
then unmounted and observed the driver diagnostic
`reservations=7907 peak_active=3 conflicts=0`. The required overlap floor is
two. The resulting image and recovery fixtures passed CLEAN scrubs, and the
same job reduced its deliberately fragmented 32 MiB file from 193 extents to
one without changing content or retained metadata.

These results qualify the integration patches themselves; unpatched
distribution GNOME Disks/UDisks/libblockdev packages require distribution or
upstream adoption before they expose the InfiltratorFS format row.

## 2026-08-31 driverless Windows Explorer bridge qualification

Release 0.18.26 driverless Windows mounting was qualified against exact source
commit `a4eb8ffe4c47ef36e256a0efa617ff19f7b61985`, Build and conformance workflow
run `33333450633`, Windows job `99316218768`. The Windows runner:

- built the portable core, Win32 raw/image adapter, Explorer bridge and transfer application with MSVC and passed all 19 Windows/portable CTest tests;
- opened a Linux-created Format 0.17 image successfully on Windows;
- started the ProjFS provider and exercised it from a separate Windows client process, matching normal Explorer/application access rather than provider-internal I/O;
- hydrated and byte-verified a Linux-created file through the mapped drive, overwrote that existing projected file from Windows, and verified the edited data persisted back into InfiltratorFS;
- created and wrote a new Windows file, persisted its close-time contents into InfiltratorFS, renamed it and created an InfiltratorFS hard link to it;
- created a directory tree, hydrated a child, renamed the directory and rewrote the child under its new Windows path while retaining the correct InfiltratorFS object;
- created and deleted another Windows file and verified it was absent from the portable namespace afterward; and
- reopened the resulting state through the portable core and verified data equality, shared hard-link object identity/link counts and delete persistence.

The runtime trace showed successful ProjFS overwrite, pre-convert-to-full, new-file, modified-close, rename, hard-link and deleted-close notifications, all returning success. This qualification covers the interim user-mode Explorer bridge; it does not claim native Windows kernel-filesystem, Cache Manager, security-descriptor or boot-volume support.

## 2026-09-01 media-aware placement qualification

Media-aware allocation was qualified without changing on-disk Format 0.17.
The final allocator media-detection code commit was
`fe6b11c984867b4fb23c937b8468924cca4e8377`; later source through
`bf123d2cc98aef8b848386c4f26cc93a04fe4dc3` changed policy guards and CI
concurrency but not media scoring semantics.

Build and conformance workflow run `33454396856` passed on exact source
`bf123d2cc98aef8b848386c4f26cc93a04fe4dc3`, including Linux, Clang,
ASan/UBSan, GCC static analysis, native Linux packages and Windows
qualification.

Native Linux workflow run `33454396842` passed both kernel families used by
the project: the self-hosted Linux 7.0 running-kernel compile and the hosted
Linux 6.17 generic/DKMS/running-kernel builds. The hosted job then passed the
full native read-write/remount/scrub qualification, online defragmentation, and
a dedicated media-profile qualification. Automatic detection on the hosted
loop-backed device resolved `media=nonrotational media_source=auto` and
reported:

- sequential workload decisions: 10,499;
- random/in-place CoW decisions: 4,401;
- direct sparse decisions: 1;
- media-aware scored selections: 6,572;
- best-fit selections: 4,402;
- successful reservations: 3,474;
- peak simultaneous reservations: 2; and
- reservation conflicts: 0.

The forced rotational profile exercised 24 sequential, 96 random and one
direct-sparse decision and reported
`media_rotational_scored=97 best_fit=0`. The forced non-rotational profile
ran the same workload and reported
`media_nonrotational_scored=97 best_fit=97`. Both images unmounted normally
and scrubbed `CLEAN`. This demonstrates the intended policy difference:
rotational media makes physical distance the primary cost, whereas
non-rotational media makes free-run preservation/best-fit primary where
appropriate. The mount option accepts
`media=auto|rotational|nonrotational|balanced`, and the resolved profile is
visible in mount options and driver telemetry. Zoned block devices fail closed
until explicit zone-write-pointer allocation is implemented.

The ordinary hosted 4,000 x 4 KiB random-overwrite phase completed in 63.246
seconds (63.2 writes/second), the 512 MiB verified read completed at
126.80 MiB/second, and the 200-fsync phase completed at 776.7 fsyncs/second.
These numbers are qualification telemetry for that hosted runner rather than
cross-filesystem benchmark claims.

Heavy filesystem workflow run `33454248279` built the exact final allocator
code on Linux 6.17. Its near-full/fragmentation/endurance job passed after a
300-second mixed workload completing 32,607 operations at 108.6
operations/second with 4,074 fsyncs. Free space was driven to 8.94%, the
hole-punch/refill cycle passed, durable read-only remount verification passed,
and both offline scrubs reported zero checksum errors, zero metadata errors and
`CLEAN`. The separate million-file/1 TiB job in that heavy run is
development-scale coverage and is not required to establish the media-profile
selection contract recorded here.

The media policy is deliberately volatile. Linux auto-detects rotational state
from the block-device API with a compatibility path for Linux 6.x and 7.x;
operators can override the profile per mount. No media type, score, cursor or
device assumption is written into Format 0.17.

## 2026-09-01 workload-aware placement qualification

Locality scoring and workload-aware placement were qualified without changing
on-disk Format 0.17. The final allocator-code commit was
`69c66ad7fa28b9308729f0181c270cd78a94ea59`; later commits in the release
series changed tests, workflow gating and documentation but not the allocator
semantics.

Heavy filesystem workflow run `33448454593` built that exact native module.
Its near-full/fragmentation/endurance job passed after:

- coarse, refill and tiny-run fragmentation with free space driven below 10%;
- 300 seconds of concurrent mixed I/O completing 31,859 operations at
  106.1 operations/second;
- random overwrite/readback, sequential append, rename/xattr churn, sparse and
  high-offset mutation, links and repeated durability publication;
- an additional hole-punch/refill cycle;
- durable read-only remount verification; and
- two offline scrubs reporting zero checksum errors, zero metadata errors and
  `CLEAN`.

Native kernel workflow run `33448866530` then passed the full mounted
read-write/remount/scrub and online-defragmentation qualification with the same
allocator semantics. Its unmount telemetry demonstrated that the new policy was
actually exercised rather than merely present in source:

- sequential workload decisions: 9,705;
- random/in-place CoW decisions: 4,401;
- direct write-beyond-EOF sparse decisions: 1;
- locality-scored extent selections: 6,432;
- best-fit random/sparse selections: 4,402;
- successful parallel reservations: 3,469;
- peak simultaneous reservations: 3; and
- reservation conflicts: 0.

The mounted 4,000 x 4 KiB random-overwrite phase completed in 86.925 seconds
(46.0 writes/second), compared with 83.226 seconds (48.1 writes/second) in the
0.18.30 hosted kernel qualification, a 4.4% elapsed-time increase while adding
best-fit/locality scoring. The complete ordinary Build and conformance workflow
run `33448969940` also passed Linux, Clang, ASan/UBSan, GCC static analysis,
native packages and Windows qualification.

The placement policy remains entirely volatile. Sequential EOF growth may
consume an exact-adjacent streaming reservation or score nearby free extents by
physical distance and remaining tail length. Random CoW and direct sparse
growth select the tightest suitable free extent before locality, preserving
larger contiguous runs for streaming writes. A pre-lock streaming reservation
is rejected if the authoritative in-lock classification is no longer
sequential.

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

## 2026-09-02 native adaptive compression qualification

Native IAC1 v1 compression was finalized against source commit
`4d7f259edc8191918f5258e0397817dddf937baf` (Format 0.17). The exact current
Build and conformance workflow run `33604589517` passed the Linux full suite,
Clang conformance, ASan/UBSan, GCC static analyzer, local Linux userspace,
Windows native app/portable core and native Linux package jobs.

The new representative compression gate exercised eight deterministic 256 KiB
workload classes: source/text, executable/library patterns, office/document
data, database records, VM/zero-heavy storage, structured binary data, mixed
small-file/configuration content and already-compressed/encrypted-style
high-entropy data. IAC1 attempted all seven compressible classes, saved
filesystem blocks on all seven, and correctly skipped the high-entropy class.

Aggregate filesystem allocation was 2,097,152 bytes uncompressed, 401,408 bytes
with IAC1 and 626,688 bytes with the retained LZ4 reference baseline. That is
80.86% physical-block saving for IAC1 versus 70.12% for LZ4 on the maintained
qualification corpus. IAC1 was materially slower to encode than LZ4 on these
hosted measurements, but delivered the stronger aggregate space result and an
especially large advantage on structured-binary data while retaining a fixed
131,072-byte scratch bound, deterministic output and bounded independent
compression units. The filesystem only selects a compressed representation
when it saves physical blocks, so this codec trade-off does not impose storage
expansion on incompressible content.

The compression conformance surface also includes compressed portable/native
read-write, partial CoW overwrite, snapshots, reflinks, truncate, hole punch,
`fallocate` reservation semantics, hard-link/unlink lifetime, paged extents,
logical allocation reporting, mmap/page-cache reads, scrub and corrupt-stream
rejection. LZ4 remains decodable as codec identifier 1; automatic Format 0.17
writes use IAC1 v1 as codec identifier 2.
