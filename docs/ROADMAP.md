<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Roadmap

## Phase 0 — current vertical slice

- [x] Define design principles.
- [x] Define format 0.1 superblock/checkpoint.
- [x] Create three physically separated checkpoints.
- [x] Implement authoritative allocation bitmap.
- [x] Implement 128-bit filesystem and object IDs.
- [x] Implement checksummed root-directory object.
- [x] Implement `mkfs.infilfs`.
- [x] Implement `infilfs-inspect`.
- [x] Add optional FUSE3 read-only mount of the empty root.
- [x] Add image-based smoke test.

## Phase 1 — actual files and directories

- [ ] Define persistent object index.
- [ ] Define variable-length directory entries.
- [ ] Define extent records.
- [ ] Allocate and free extents through the bitmap.
- [ ] Implement lookup, create, mkdir, unlink and rename.
- [ ] Implement file read and write.
- [ ] Add timestamps, mode bits, ownership and Linux permission semantics.
- [ ] Add robust bounds checking and corrupted-image rejection tests.

Milestone: copy a directory tree onto an InfiltratorFS image, unmount it, remount it and recover byte-identical data.

## Phase 2 — transactional metadata

- [ ] Replace mutable metadata updates with copy-on-write objects.
- [ ] Add generation-aware metadata roots.
- [ ] Publish commits through checkpoint generations.
- [ ] Add crash-injection test harness.
- [ ] Prove old generation remains mountable after interruption at every simulated write boundary.

Milestone: writable crash-consistent filesystem without offline journal replay.

## Phase 3 — integrity

- [ ] Add data-extent checksums.
- [ ] Introduce stronger 256-bit checksum algorithm while retaining algorithm IDs.
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

- [ ] Free-extent index as rebuildable accelerator over the bitmap.
- [ ] Scalable object and directory trees.
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

Global synchronous deduplication, distributed/network filesystem semantics and application-visible transaction APIs are not first-generation requirements. They remain possible research directions once the core filesystem is proven.
