<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Roadmap

Pre-1.0 development is current-format-only. A new development format may supersede the current format without a backward reader or migration path. Backward compatibility begins with the first stable release.

## Design principles and scope guardrails

InfiltratorFS is a greenfield filesystem intended for future general-purpose operating systems, not a compatibility clone of an older filesystem. Before 1.0, prefer the cleanest long-term format and semantics over preserving development-format compatibility.

The roadmap is guided by these architectural commitments:

- Persistent 128-bit object identity is fundamental; pathnames are namespace mappings rather than file identity.
- Updates are generation-based copy-on-write transactions with atomic publication. Snapshots, retained history, reflinks and recovery should remain consequences of that common model rather than separate incompatible mechanisms.
- Data and metadata integrity are first-class requirements. Normal operation must verify stored state, scrub must be able to validate the complete reachable graph, and recovery must fail closed when correctness cannot be established.
- Metadata structures must scale without a fixed global inode table or other small-volume assumptions.
- The persistent format and portable core define filesystem semantics. Linux, Windows and future operating systems are native adapters over that model, not the definition of it.
- Namespace and security semantics should be portable and Unicode-native rather than permanently encoding one operating system's UID/GID, SID, ACL or filename model as canonical.
- Storage layout is policy, not identity. Allocation, workload classification, media-aware placement and similar tuning should remain replaceable runtime policy wherever they do not need persistent semantics.
- A single-device volume must remain a complete, understandable and efficient filesystem in its own right. Multi-device placement, protection classes, replication and parity must remain optional layers and must not distort the single-device core.
- Advanced capabilities such as compression, online defragmentation, encryption, case-folding and named streams belong when they provide broad general-purpose value, but should remain optional or policy-driven where possible.
- New roadmap features should either strengthen a core primitive or provide a broadly useful filesystem capability. Avoid adding narrowly specialised machinery merely to increase the feature count.
- Do not inherit historical filesystem limits, codec choices, constants or semantics merely because they are conventional. Every persistent hard limit or default must have an explicit current rationale, scale analysis and qualification plan; implementation conveniences remain provisional until deliberately adopted.

The intended core can be summarized as: stable object identity; transactional generations; end-to-end integrity; cheap history; scalable metadata; portable security; and native operating-system adapters. Features that build naturally on those primitives are preferred over unrelated special cases.

## Completed foundation

- [x] Three physically separated checkpoints and generation-based recovery.
- [x] Authoritative allocation bitmap and copy-on-write transactions.
- [x] 128-bit persistent filesystem/object identities.
- [x] CRC64 metadata integrity and SHA-256 file-data integrity.
- [x] Portable C17 core with callback-based storage services.
- [x] Stable `infs_status` error contract and platform-neutral packed format.
- [x] Exact-format conformance, malformed-image rejection and crash testing.

## Completed Format 0.17 storage features

- [x] UTF-8 namespace and portable/POSIX metadata split.
- [x] Inline small files.
- [x] Sparse files and hole punching.
- [x] Shared extents and reflinks.
- [x] Generation-aware object-index and directory trees plus paged extent metadata.
- [x] Symbolic links and regular-file hard links.
- [x] Named read-only snapshots and retained historical generations.
- [x] Online snapshot-coordinated scrub.
- [x] Raw forensic metadata discovery.
- [x] Operation-level transaction savepoints.
- [x] Runtime hashed metadata indexes.
- [x] Sequential allocation and checksum hot-path performance work.

## Completed native Linux filesystem surface through release 0.18.32

- [x] Native out-of-tree Linux VFS filesystem driver `infiltratorfs.ko`.
- [x] DKMS packaging and host-kernel rebuild/install path.
- [x] Native Format 0.17 read-only lookup, enumeration and file reads.
- [x] Native read-write create, mkdir, mknod, setattr and extent-backed writes.
- [x] Link/symlink, rename/unlink/rmdir and persistent metadata changes.
- [x] Random writes, sparse growth, high-offset writes, truncate and `fallocate`.
- [x] Hole punching and logical/allocated block reporting.
- [x] Crash-safe open-unlink/replacement lifetime and mount-time orphan recovery.
- [x] Persistent Linux `user.*`, `trusted.*`, `security.*` and `system.*`
  xattr handlers, plus FIFO/socket/character/block node identity.
- [x] Page-cache, readahead and shared writable `mmap` integration.
- [x] Writable live generations while named snapshots retain older generations.
- [x] Native transaction publication at durability boundaries.
- [x] Metadata and file-data integrity checking on the native path.
- [x] Native `mount.infiltratorfs` helper using `mount -i -t infiltratorfs`.
- [x] InfiltratorFS Manager native block-device and loop-image mounting.
- [x] udev/UDisks filesystem identification and desktop visibility.
- [x] Native-only Debian and `.run` packaging with no FUSE runtime dependency.
- [x] Removal of the legacy FUSE implementation from the current source/product path.
- [x] Mounted remount/scrub qualification for the complete migrated native surface.
- [x] Release gate that installs the generated `.deb`, mounts `FSTYPE=infiltratorfs`, writes/reads non-zero data, syncs, unmounts and requires a clean scrub.
- [x] Multi-process locking/concurrency qualification across same-directory namespace mutation, shared-inode writes/fsync, same-inode xattr readers/writers and open-unlink lifetime.
- [x] Native metadata-tree alias/cycle rejection and kernel-stack hardening for allocation-map/tree traversal.
- [x] Safe package upgrade while an administrator emergency module-disable policy remains active.

## Linux next work

- [x] Dirty-checkpoint fallback and writable checkpoint-replica healing on mounted images.
- [x] Standard Linux xattr namespaces and policy-aware error-path qualification.
- [x] Directory/symlink/special-object allocation-reporting qualification.
- [x] Explicit mounted regression coverage for every formerly mounted semantic path,
  including crash-orphan recovery, open-handle rename/replace, 4,000 random
  overwrites and repeated fsync publication.
- [x] Broader locking/concurrency qualification.
- [x] Millions-of-files and large-volume mounted stress tests, including
  1,000,000 files, 100,000 unlink/recreate operations and a separate 1 TiB
  sparse-volume qualification with durable remount verification and CLEAN scrub.
- [x] Wider near-full, fragmentation and long-running mixed-workload tests,
  including bounded near-full/refill cycles, five minutes of concurrent mixed
  I/O, durable read-only remount verification and two CLEAN scrubs.
- [x] Native fragmentation/optimisation metrics and online defragmentation
  through the per-file ioctl ABI and `infilfs-optimize`, with mounted identity,
  content, xattr, timestamp, hard-link, snapshot, scrub and remount qualification.
- [x] libblockdev/UDisks/GNOME Disks formatter integration through
  version-pinned upstream patches, a conventional `mkfs.infiltratorfs` helper
  and CI that builds all three projects and executes generic format, label and
  probe end to end. Distribution adoption remains separate from the shipped
  integration implementation.

The completed items above are enforced by native mounted CI and the partition-22
destructive qualification harness.

On 2026-08-29, all 56 currently checked roadmap entries were requalified at
source commit `075aed9c737fb38cc408d752736a97773dc2a035` through the complete
portable/CI suite plus destructive physical `/dev/mmcblk0p22` testing. The
physical harness passed 69/69 checks, four additional concurrency rounds passed,
and the final generation-3695 scrub was CLEAN with zero checksum or metadata
errors. See [QUALIFICATION.md](QUALIFICATION.md) for the evidence record and
performance telemetry.

On 2026-08-30, the first three formerly unchecked Linux qualification items
passed on exact `main` at commit
`a40a9a9a8f12b4789c3e582b92abf5678a07e79e`. The dedicated million-file/1 TiB
and near-full/endurance jobs passed, as did mounted native online-defragmentation
qualification. See [QUALIFICATION.md](QUALIFICATION.md) for the evidence record.

On 2026-08-31, the final two selected items passed exact-source qualification.
The pinned libblockdev/UDisks/GNOME Disks stack built successfully and its
generic libblockdev path formatted, labelled and probed a real Format 0.17
image. The hosted native kernel gate also hash-verified 16 concurrent 4 MiB
writers and observed three simultaneously active allocation reservations. See
[QUALIFICATION.md](QUALIFICATION.md) for the commit and workflow evidence.

On 2026-09-01, locality scoring and workload-aware placement passed mounted
native qualification. The final allocator code at
`69c66ad7fa28b9308729f0181c270cd78a94ea59` survived the near-full,
fragmentation and five-minute mixed-I/O qualification in workflow run
`33448454593`, including two CLEAN offline scrubs. The mounted kernel gate in
run `33448866530` then exercised all three authoritative workload classes and
reported `workload_seq=9705`, `workload_random=4401`,
`workload_sparse=1`, `locality_scored=6432` and `best_fit=4402`, while
retaining a peak of three concurrent reservations with zero conflicts. See
[QUALIFICATION.md](QUALIFICATION.md) for the evidence and performance comparison.

Also on 2026-09-01, media-aware placement passed exact-source qualification.
Current-head native workflow run `33454396842` passed Linux 7.0 local compile,
Linux 6.17 hosted DKMS/running-kernel builds, the full mounted native suite, and
forced rotational/non-rotational placement profiles. Rotational scoring recorded
97 seek-first scored selections with `best_fit=0`; non-rotational scoring
recorded 97 scored selections with `best_fit=97`. Both profile images scrubbed
CLEAN. Heavy run `33454248279` passed the near-full five-minute mixed workload
on the same allocator semantics with 32,607 operations, two CLEAN scrubs and no
checksum or metadata errors. See [QUALIFICATION.md](QUALIFICATION.md).

## Scale and performance

- [x] Scalable generation-aware object-index tree.
- [x] Scalable directory trees.
- [x] Rebuildable free-extent index.
- [x] Parallel native data-allocation model with 64 volatile reservation
  shards, object-stable placement and mounted multi-writer qualification.
- [x] Locality scoring and workload-aware placement.
- [x] Media-aware placement.
- [ ] InfiltratorFS-native adaptive per-extent compression designed from current compression research; retain LZ4 as a non-default development/reference codec and keep the on-disk codec model extensible. See [COMPRESSION.md](COMPRESSION.md).

## Administration and recovery

- [ ] Filesystem resize support, including online grow and safely bounded shrink.
- [ ] Native user, group and project/directory-tree quotas.
- [ ] Deterministic repair-capable filesystem checker for corruption that can be repaired unambiguously, while retaining fail-closed behaviour where correctness cannot be established.
- [ ] Snapshot restore and rollback for selected files/directories and whole-volume recovery to a retained generation.

## Security and protection

- [ ] Versioned portable security objects with stable typed principals and ACL entries.
- [ ] Generic access-right vocabulary independent of POSIX and Windows constants.
- [ ] Linux UID/GID/mode and POSIX ACL mapping policy.
- [ ] Windows SID/security-descriptor mapping policy.
- [ ] Preservation rules for platform-specific security metadata not understood by another adapter.
- [ ] Portable named-attribute/named-stream objects distinct from adapter sidecars.
- [ ] Protection classes and multi-device placement.
- [ ] Replication and/or parity.
- [ ] Encryption domains and key wrapping.
- [ ] Authenticated metadata for encrypted volumes.

See `SECURITY.md` for the intended cross-platform model. Current POSIX compatibility metadata and Linux xattr sidecars are not the final portable security representation.

## Namespace portability

- [x] 1023-byte UTF-8 component-name limit with portable and native boundary qualification.
- [ ] Versioned Unicode normalization policy.
- [ ] Optional case-folded directory policy.
- [ ] Cross-platform removable-volume filename profile.
- [ ] Generic reparse/typed-extension objects.
- [ ] Generic named data streams/extended metadata model.

## Operating-system adapters

- [x] Native Linux VFS adapter is the current mounted reference implementation.
- [x] Win32 image/device and bounded raw-partition storage backend.
- [x] Windows storage/partition conformance harness.
- [x] Windows formatter, root transfer/listing and scrub application.
- [x] Shared Linux/Windows Format 0.17 interoperability tests.
- [x] Driverless Windows Explorer drive-letter bridge using Microsoft's inbox ProjFS, with portable-core read/write, rename, hard-link and delete persistence qualification.
- [ ] Windows attribute/security/filename adapter completion.
- [ ] Native Windows filesystem driver and native Cache Manager/I/O Manager integration.
- [ ] macOS native adapter investigation/implementation.
- [ ] BSD native adapter investigation/implementation.
- [ ] Haiku native adapter investigation/implementation.

The adapter architecture is deliberately generic: no future operating system should require redefining the persistent filesystem merely because it uses different API names for equivalent concepts. See `PLATFORM_ADAPTERS.md`.

## Deliberately deferred

Global synchronous deduplication, distributed/network filesystem semantics and application-visible transactions are not first-generation requirements.
