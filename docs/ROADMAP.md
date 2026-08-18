<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Roadmap

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
- [ ] Forensic raw-object scanner.
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

## Phase 4 — modern storage features

- [x] Sparse extents.
- [ ] Inline small files.
- [ ] Reflinks and shared extents.
- [ ] Snapshot roots and retained generations.
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
- [ ] Symbolic links and generic reparse-point objects.
- [ ] Hard links and transactional reference accounting.

## Phase 8 — operating-system integration

### Linux

- [x] POSIX file/device backend.
- [x] FUSE3 reference adapter.
- [ ] Native Linux kernel driver.
- [ ] `mount.infilfs` and standard utilities.
- [ ] Debian/Ubuntu/Linux Mint packaging.

### Windows

- [ ] Win32 image/device storage backend.
- [ ] Windows userspace conformance harness.
- [ ] Windows attribute, security and filename adapter.
- [ ] Native Windows filesystem driver.
- [ ] Windows formatter, inspector and scrub utilities.
- [ ] Shared Linux/Windows volume interoperability tests.

## Deliberately deferred

Global synchronous deduplication, distributed/network filesystem semantics and application-visible transactions are not first-generation requirements.
