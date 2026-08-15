<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Roadmap — Part 2

- [x] Introduce SHA-256 file-data checksums while retaining explicit algorithm IDs.
- [ ] Add duplicate-metadata/data recovery where trustworthy copies exist.
- [ ] Build forensic scanner that enumerates recognizable objects from a damaged image.

Milestone: a committed file-data overwrite has old-or-new crash semantics, every allocated file block is independently verifiable, and a full-volume scrub can detect silent data corruption.

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

- [ ] Replace the one-block object index with a scalable generation-aware tree.
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
