<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Roadmap

## Phase 0 — format skeleton — complete

- [x] Define design principles.
- [x] Define the initial superblock/checkpoint model.
- [x] Create three physically separated checkpoints.
- [x] Implement authoritative allocation bitmap.
- [x] Implement 128-bit filesystem and object IDs.
- [x] Implement checksummed metadata objects.
- [x] Implement `mkfs.infilfs`.
- [x] Implement `infilfs-inspect`.
- [x] Add image-based smoke testing.

## Phase 1 — actual files and directories — core complete

- [x] Define persistent object index.
- [x] Define variable-length directory entries.
- [x] Define extent records.
- [x] Allocate and free extents through the bitmap.
- [x] Implement lookup, create, mkdir, unlink, rmdir and rename.
- [x] Implement file read, write and truncate.
- [x] Store mode bits, UID/GID, link counts and nanosecond timestamps.
- [x] Add direct-image utility for persistence testing without FUSE.
- [x] Convert FUSE front end to the writable core engine.
- [x] Add bounds checking and corrupted-image rejection tests.
- [x] Add byte-identity, zero-fill, truncate and cross-directory-move tests.
- [x] Run the Phase 1 suite under AddressSanitizer and UndefinedBehaviorSanitizer.
- [ ] Run the mounted copy-tree milestone on Linux Mint with real FUSE3 and `/dev/fuse`.

Milestone: mount a formatted image, copy a directory tree through ordinary Linux file operations, unmount it, remount it and recover byte-identical data.

## Phase 2 — transactional metadata

- [ ] Replace mutable metadata updates with copy-on-write objects.
- [ ] Replace the single-block object index with a generation-aware scalable tree.
- [ ] Add generation-aware metadata roots.
- [ ] Publish commits atomically through checkpoint generations.
- [ ] Separate allocation reservation from committed allocation state.
- [ ] Add crash-injection test harness.
- [ ] Prove generation N remains mountable after interruption at every simulated write boundary while constructing N+1.

Milestone: writable crash-consistent filesystem without offline journal replay.

## Phase 3 — integrity

- [ ] Add data-extent checksums.
- [ ] Introduce a stronger 256-bit checksum algorithm while retaining algorithm IDs.
- [ ] Add mounted metadata scrub.
- [ ] Add mounted data scrub.
- [ ] Add duplicate-metadata recovery where trustworthy copies exist.
- [ ] Build forensic scanner that enumerates recognizable objects from a damaged image.

## Phase 4 — modern storage features

- [ ] Sparse extents.
- [ ] Inline small files.
- [ ] Reflinks/shared extents.
- [ ] Snapshot roots and retained generations.
- [ ] Native historical undelete policy.
- [ ] Per-extent compression.
- [ ] Workload-aware allocation hints and automatic heuristics.
- [ ] Media-aware allocation policy.

## Phase 5 — scale and performance

- [ ] Scalable directory trees.
- [ ] Free-extent index as rebuildable accelerator over the bitmap.
- [ ] Parallel allocation paths.
- [ ] Allocation locality scoring.
- [ ] Fragmentation/contiguity metrics.
- [ ] Online optimise/defragment operation.
- [ ] Large-volume and millions-of-files stress testing.

## Phase 6 — protection and security

- [ ] Protection classes.
- [ ] Multi-device placement model.
- [ ] Replication and/or parity design.
- [ ] Encryption domains and key wrapping.
- [ ] Metadata authentication suitable for encrypted volumes.

## Phase 7 — native Linux integration

- [ ] Stabilise userspace engine and on-disk format.
- [ ] Write native Linux kernel filesystem driver.
- [ ] Add `mount.infilfs` helper and standard filesystem utilities.
- [ ] Package for Debian/Ubuntu/Linux Mint.
- [ ] Maintain FUSE implementation as reference/debugging implementation.

## Deliberately deferred

Global synchronous deduplication, distributed/network filesystem semantics and application-visible transaction APIs are not first-generation requirements. They remain possible research directions once the core filesystem and crash-consistency model are proven.
