<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Roadmap

Pre-1.0 development is current-format-only. A new development format supersedes earlier formats without requiring backward readers or migration code. Backward compatibility begins with the first stable release.

## Phase 0 — format skeleton — complete

- [x] Three physically separated checkpoints.
- [x] Authoritative allocation bitmap.
- [x] 128-bit volume and object IDs.
- [x] Checksummed metadata objects.
- [x] Formatter, inspector and image-based smoke test.

## Phase 1 — files and directories — core complete

- [x] Persistent object index, variable directory entries and extents.
- [x] Lookup, create, mkdir, unlink, rmdir and rename.
- [x] Read, write, grow and shrink.
- [x] Direct-image test utility and writable Linux FUSE adapter.
- [x] Bounds checking and corrupted-image rejection.
- [x] Linux Mint mounted copy-tree/remount/byte-identity milestone on 15 August 2026.

## Phase 2 — transactional metadata — core complete

- [x] Copy-on-write metadata, object index and allocation bitmap.
- [x] Generation-based checkpoint publication.
- [x] Deferred reclamation and mixed-checkpoint healing.
- [x] Deterministic pre/post-commit crash tests.
- [x] Repeated-CoW leak tests.

## Phase 3 — data integrity — core complete

- [x] Independent per-logical-block data checksums.
- [x] SHA-256 data verification.
- [x] Copy-on-write committed data replacement.
- [x] Read-path verification and full-volume scrub.
- [x] Deliberate corruption and data-crash tests.
- [x] Multi-object checksum chains.
- [ ] Duplicate metadata/data recovery.
- [x] Validate a checkpoint's referenced generation graph during recovery and fall back to an older valid committed generation when the newest candidate graph is corrupt.
- [x] Forensic raw-object scanner.
- [ ] Snapshot-coordinated online scrub.

## Phase 3.5 — platform-neutral foundation — complete

- [x] Define InfiltratorFS as platform-neutral rather than Linux-specific.
- [x] Publish incompatible format 0.5.
- [x] Replace fundamental POSIX stat metadata with common attributes.
- [x] Add birth time, portable flags and future security/xattr references.
- [x] Isolate POSIX permissions and UID/GID as adapter metadata.
- [x] Require valid UTF-8 names and labels.
- [x] Replace Linux file-descriptor I/O in the core with storage callbacks.
- [x] Replace core `off_t`, `ssize_t`, `struct stat`, `mode_t`, `uid_t`, `gid_t` and `timespec` interfaces with fixed-width types.
- [x] Add compiler-neutral endian helpers and packed-record declarations.
- [x] Add a portable in-memory storage-backend persistence test.
- [x] Keep POSIX I/O and FUSE outside the core engine.

## Phase 3.6 — Format 0.5 conformance — complete

- [x] Publish implementation release 0.5.1 without changing on-disk Format 0.5.
- [x] Add stable operating-system-neutral `infs_status` values.
- [x] Map POSIX `errno` only at the adapter boundary.
- [x] Add exact structure-size and field-offset assertions.
- [x] Add an independently constructed byte-exact checkpoint test.
- [x] Add strict valid/invalid UTF-8 conformance vectors.
- [x] Add deterministic complete-volume malformed-image tests.
- [x] Add full Linux CI and Windows/MSVC portable-core CI.

## Phase 4.1 — Format 0.6 sparse files — complete

- [x] Publish incompatible Format 0.6 with an explicit sparse-extents feature bit.
- [x] Add zero-storage hole extents and exact allocated-size reporting.
- [x] Make truncate growth allocate no data or checksum blocks.
- [x] Allocate only blocks touched by writes into holes.
- [x] Add sorted sparse checksum-object chains for high-offset writes.
- [x] Add full and partial hole punching in the core API, direct-image tool and FUSE adapter.
- [x] Preserve copy-on-write publication and SHA-256 verification for sparse data.
- [x] Add portable 1 TiB logical-file, malformed-extent, crash and leak tests.

## Phase 4.2 — implementation 0.6.1 Format 0.6 hardening — complete

- [x] Preserve the Format 0.6 packed layout and feature identity while publishing an implementation bugfix release.
- [x] Validate exact ownership of every committed allocated block.
- [x] Validate the complete namespace graph: unique names, target type/identity, parent links, link counts and root reachability.
- [x] Reject stored `.` / `..`, dangling namespace references and unintended multiple references while hard links are unsupported.
- [x] Validate that every indexed checksum object is reachable exactly once from its owning file chain.
- [x] Enforce canonical zero padding and currently reserved metadata fields.
- [x] Enforce unknown read-only-compatible features as read-only and reject them for writable open.
- [x] Generate nonzero collision-checked persistent object IDs.
- [x] Implement atomic POSIX rename replacement for files and empty directories.
- [x] Open FUSE backing storage genuinely read-only for read-only mounts.
- [x] Refuse formatting mounted/held real block targets even when `--force` is supplied.
- [x] Publish formatter checkpoints only after referenced metadata is durably written.
- [x] Expand regression coverage and compile/test the FUSE adapter under Linux, Clang, ASan/UBSan and GCC `-fanalyzer` gates.

## Phase 4.3 — implementation 0.6.2 source hardening — complete

- [x] Reject a committed bitmap that is too small to represent `total_blocks` before any bitmap traversal.
- [x] Require mutation-capable storage backends to provide write, flush, randomness and clock callbacks.
- [x] Preserve concrete `infs_status` failures through checksum, write, truncate, punch, enumeration, remove, rename and attribute-update paths.
- [x] Fail transactions on clock-service errors instead of silently committing zero timestamps.
- [x] Add malformed-open/backend-capability regression coverage.
- [x] Enforce directory traversal semantics for trailing slash, `.` and `..` path components and enforce the core path-length limit.
- [x] Normalize pre-epoch FUSE timestamps, overflow-check time/offset conversions and reject oversized I/O before mutation.
- [x] Reopen real formatter block targets with `O_EXCL`, then revalidate identity and geometry before destructive writes.
- [x] Keep on-disk Format 0.6 unchanged.

## Phase 4.4 — implementation 0.6.3 recovery hardening — complete

- [x] Validate each physical checkpoint's complete referenced generation graph before accepting it for recovery.
- [x] Try committed checkpoint candidates in descending generation order and fall back only when the newer candidate graph is structurally corrupt.
- [x] Preserve I/O, memory and unsupported-feature failures rather than hiding them behind an older-generation fallback.
- [x] Heal all physical checkpoint replicas to the selected generation after writable fallback recovery.
- [x] Preserve trailing-slash directory semantics across rename source and destination paths.
- [x] Return `INVALID_ARGUMENT` rather than `READ_ONLY` for a null internal transaction volume.
- [x] Fail formatting when the initial realtime-clock query fails instead of silently creating zero initial timestamps.
- [x] Add deterministic portable checkpoint-fallback, non-masked-I/O and rename-path regression coverage.
- [x] Verify and retain libfuse's documented high-level `readdir` mode instead of introducing unnecessary synthetic directory cookies.
- [x] Keep on-disk Format 0.6 unchanged.

## Phase 4.5 — implementation 0.6.4 commit and concurrency safety — complete

- [x] Refuse writable recovery when any physical checkpoint is unreadable and may contain the only newer committed generation.
- [x] Preserve read-only inspection through surviving checkpoint replicas without modifying media.
- [x] Disable all further mutation and require close-and-reopen recovery when the first checkpoint publication write or durability flush has an indeterminate outcome.
- [x] Enforce shared read-only and exclusive writable POSIX storage locks.
- [x] Coordinate the formatter with mounted volumes and other tools through the same exclusive storage lock.
- [x] Add deterministic mixed-generation, indeterminate-commit and POSIX lock-contention regression coverage.
- [x] Document the high-level FUSE adapter's incomplete open-handle semantics as unsupported.
- [x] Keep on-disk Format 0.6 unchanged.

## Phase 4.6 — implementation 0.6.5 Linux Mint desktop packaging — complete

- [x] Add a Mint application-menu launcher for InfiltratorFS management.
- [x] Support image creation, formatting, inspection, scrubbing, mounting and safe unmounting through Zenity.
- [x] List removable partitions created by Mint Disks while excluding fixed disks from the GUI.
- [x] Gate raw-device operations through PolicyKit and a constrained privileged helper.
- [x] Open mounted volumes in the normal desktop file manager.
- [x] Package the manager, launcher and runtime dependencies in the Debian release asset.
- [x] Keep on-disk Format 0.6 unchanged.

## Phase 4.7 — implementation 0.7.0 inline small files — complete

- [x] Publish Format 0.7 and define the `INFS_INCOMPAT_INLINE_DATA` feature bit without changing existing packed structure sizes or offsets.
- [x] Keep non-empty regular files up to 3,840 bytes inside their existing file metadata object on inline-enabled volumes.
- [x] Store an independent SHA-256 digest beside inline bytes while retaining the enclosing object's CRC64 metadata checksum.
- [x] Preserve zero-filled unwritten ranges and hole-punch semantics while a file remains inline.
- [x] Promote inline files transactionally to ordinary CoW extents when growth crosses 3,840 bytes.
- [x] Fold extent-backed files back inline when truncate shrinks them to 3,840 bytes or less.
- [x] Reclaim external data and checksum blocks when folding a file inline.
- [x] Permit extent-only current-format test volumes with the inline-data bit clear.
- [x] Add portable threshold, promotion, folding, punch, scrub and remount conformance coverage.

## Phase 4.8 — implementation 0.8.0 shared extents — complete

- [x] Define the `INFS_INCOMPAT_SHARED_EXTENTS` feature bit.
- [x] Create file reflinks that share ordinary extent data blocks.
- [x] Preserve independent checksum chains for reflinked files.
- [x] Break sharing through CoW on later writes.
- [x] Preserve shared blocks across truncate, hole punching, inline folding, rename replacement and unlink.
- [x] Validate shared physical ownership only when the feature is enabled.
- [x] Add deterministic portable reflink, reclamation, scrub and remount coverage.

## Phase 4.9 — Format 0.8 paged metadata — complete

- [x] Define version-2 directory/index heads and checksummed metadata pages.
- [x] Publish incompatible Format 0.8 with `INFS_INCOMPAT_PAGED_METADATA`.
- [x] Store directory entries and object-index entries across bounded page arrays.
- [x] Validate page ownership, generations, entry counts, checksums and graph reachability.
- [x] Add next-fit allocation, object-ID caching and batched checksum updates.
- [x] Add bounded deferred transaction publication for Linux FUSE and Windows transfer writes.
- [x] Add paged metadata, buffered-write and Linux-to-Windows interoperability coverage.

## Phase 4.10 — implementation 0.10.0 operational completeness — complete

- [x] Expose Format 0.8 reflink creation through `infilfs-tool`.
- [x] Permit Linux Manager selection of fixed non-system partitions while retaining whole-disk and active-system exclusion.
- [x] Register real mounted-FUSE copy, rename, permission, sparse, punch and remount coverage when `/dev/fuse` is available.
- [x] Align architecture, format, conformance and roadmap documentation with Format 0.8.
- [x] Keep on-disk Format 0.8 unchanged.

## Phase 4.11 — implementation 0.11.0 forensic metadata discovery — complete

- [x] Scan every complete physical block through the portable storage interface.
- [x] Independently authenticate checkpoints, object heads, directory pages and index pages.
- [x] Classify current, stale and orphaned metadata when the live allocation map survives.
- [x] Continue raw discovery with unknown allocation state when all checkpoints are damaged.
- [x] Provide stable human-readable TSV and JSON Lines output.
- [x] Expose read-only forensic scanning through the Linux Mint Manager.
- [x] Reject damaged candidate records and add deterministic CoW-orphan/checkpoint-loss coverage.
- [x] Keep on-disk Format 0.8 unchanged.

## Phase 4.12 — implementation 0.12.0 persistent FUSE file handles — complete

- [x] Allocate stable adapter handles for FUSE create and open operations.
- [x] Route descriptor-based reads, writes, truncate, allocation and attribute updates through the retained object path.
- [x] Preserve open files across direct rename and parent-directory rename.
- [x] Preserve an open destination across atomic rename replacement.
- [x] Keep an unlinked file usable until its final descriptor closes.
- [x] Reclaim hidden handle-retention objects after final close and interrupted mounts.
- [x] Add mounted rename, replacement, unlink, read, write, truncate and fsync lifetime coverage.
- [x] Keep on-disk Format 0.8 unchanged.

## Phase 4.13 — implementation 0.13.0 standard Linux utilities — complete

- [x] Install a `mount.infiltratorfs` helper for normal `mount -t infiltratorfs` operation.
- [x] Preserve read-only/read-write selection and comma-separated FUSE mount options.
- [x] Reject unsupported helper options instead of silently changing mount semantics.
- [x] Install a read-only `fsck.infiltratorfs` front end to the complete scrub engine.
- [x] Map clean, uncorrected-corruption, operational-failure and usage results to standard fsck exit codes.
- [x] Add argument, quoting, option-forwarding and status-mapping conformance coverage.
- [x] Include both helpers in native and Debian installations.
- [x] Keep on-disk Format 0.8 unchanged.

## Phase 4.14 — implementation 0.14.0 native symbolic links — complete

- [x] Publish feature-gated Format 0.9 with symbolic-link object type 5.
- [x] Store nonempty relative or absolute UTF-8 targets inline with portable metadata.
- [x] Create, read, rename, replace, unlink and scrub symbolic links through the portable core.
- [x] Expose standard `symlink(2)` and `readlink(2)` behavior through Linux FUSE.
- [x] Add offline `symlink` and `readlink` commands and forensic classification.
- [x] Gate symbolic-link objects through their incompatible feature bit.
- [x] Add portable, mounted-FUSE, remount, corruption-boundary and Windows interoperability coverage.

## Phase 4.15 — implementation 0.15.0 regular-file hard links — complete

- [x] Publish feature-gated Format 0.10 without changing packed object layouts.
- [x] Map multiple names to one regular-file object ID, payload and checksum chain.
- [x] Update exact reference counts transactionally across link, unlink and rename replacement.
- [x] Preserve linked files across cross-directory rename, writes, scrub and remount.
- [x] Expose hard links through the portable API, `infilfs-tool` and Linux FUSE.
- [x] Reject hard links to directories and symbolic links.
- [x] Gate hard-link semantics through their incompatible feature bit.
- [x] Add portable, mounted-FUSE, corruption-boundary and Windows interoperability coverage.

## Phase 4.16 — implementation 0.16.0 snapshot roots — complete

- [x] Publish feature-gated Format 0.11 with snapshot-catalog object type 6.
- [x] Capture named immutable generation, root, object-index and bitmap images.
- [x] Preserve superseded metadata and data while any retained generation owns it.
- [x] List and browse read-only historical paths through the portable API and `infilfs-tool`.
- [x] Delete snapshots transactionally and reclaim blocks after the final historical reference.
- [x] Validate nested historical graphs and scrub retained file data.
- [x] Gate snapshot catalogs through their incompatible feature bit.
- [x] Add remount, corruption, nested-retention, CLI and reclamation coverage.

## Phase 4 — modern storage features

- [x] Sparse extents.
- [x] Inline small files.
- [x] Reflinks and shared extents.
- [x] Snapshot roots and retained generations.
- [ ] Native historical undelete policy.
- [ ] Per-extent compression.
- [ ] Workload-aware allocation.
- [ ] Media-aware placement.

## Phase 5 — scale and performance

- [ ] Scalable generation-aware object-index tree.
- [ ] Scalable directory trees.
- [ ] Rebuildable free-extent index.
- [ ] Parallel allocation and locking model.
- [ ] Locality scoring and fragmentation metrics.
- [ ] Online optimise/defragment operation.
- [ ] Millions-of-files and large-volume stress tests.

## Phase 6 — security and protection

- [ ] Versioned security objects with typed principals and ACL entries.
- [ ] Linux UID/GID/mode mapping policy.
- [ ] Windows SID/security-descriptor mapping policy.
- [ ] Extended attributes and named data streams.
- [ ] Protection classes and multi-device placement.
- [ ] Replication and/or parity.
- [ ] Encryption domains and key wrapping.
- [ ] Authenticated metadata for encrypted volumes.

## Phase 7 — namespace portability

- [ ] Versioned Unicode normalization policy.
- [ ] Optional case-folded directory policy with unambiguous comparison rules.
- [ ] Cross-platform removable-volume filename profile.
- [x] Symbolic links.
- [ ] Generic reparse-point objects.
- [x] Hard links and transactional reference accounting.

## Phase 8 — operating-system integration

### Linux

- [x] POSIX file/device backend.
- [x] FUSE3 reference adapter.
- [x] Persistent FUSE inode/open-file handles with unlink/rename lifetime conformance tests.
- [ ] Native Linux kernel driver.
- [x] `mount.infiltratorfs`, `fsck.infiltratorfs` and standard utilities.
- [x] Debian/Ubuntu/Linux Mint packaging with an application-menu manager.

### Windows

- [x] Win32 image/device and bounded raw-partition storage backend.
- [x] Windows userspace storage/partition conformance harness.
- [ ] Windows attribute, security and filename adapter.
- [ ] Native Windows filesystem driver.
- [x] Windows formatter, root transfer/listing and scrub application.
- [x] Shared Linux/Windows Format 0.11 volume interoperability tests.

## Deliberately deferred

Global synchronous deduplication, distributed/network filesystem semantics and application-visible transactions are not first-generation requirements.
