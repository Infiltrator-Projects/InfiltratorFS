<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Roadmap — Part 1

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
- [x] Run the mounted copy-tree milestone on Linux Mint with real FUSE3 and `/dev/fuse` (15 August 2026; unmount/remount and byte-identity verification passed).

Milestone: mount a formatted image, copy a directory tree through ordinary Linux file operations, unmount it, remount it and recover byte-identical data.

## Phase 2 — transactional metadata — core complete

- [x] Replace mutable metadata updates with copy-on-write objects.
- [x] Make object-index updates copy-on-write while preserving 128-bit object identity.
- [x] Make the authoritative allocation bitmap copy-on-write.
- [x] Separate transaction allocation reservations from committed allocation state.
- [x] Defer reclamation of superseded committed blocks until the new checkpoint is durable.
- [x] Publish generation N+1 through one durable checkpoint commit point, then replicate the remaining copies.
- [x] Heal mixed-generation checkpoint copies on writable open before allocating again.
- [x] Add deterministic crash-injection points before bitmap publication, after bitmap publication and after first-checkpoint publication.
- [x] Prove pre-commit interruptions mount generation N and post-commit interruption mounts N+1.
- [x] Verify repeated CoW metadata transactions reclaim superseded blocks rather than leaking free space.
- [ ] Replace the one-block object index with a scalable tree (moved to Phase 5; not required for transaction correctness).

Milestone: crash-consistent metadata publication without journal replay. Existing allocated file-data overwrites are not yet old-or-new atomic and remain a later data-integrity/data-CoW problem.

## Phase 3 — integrity — data-integrity core complete

- [x] Add per-logical-block file-data checksums stored independently from data extents.
- [x] Reserve 32-byte checksum entries so stronger 256-bit algorithms can be introduced without changing checksum-object shape.
- [x] Make committed file-data overwrites copy-on-write and publish their new extent/checksum state through the Phase 2 transaction checkpoint.
- [x] Verify file-data checksums on ordinary reads.
- [x] Add read-only `infilfs-scrub` for complete file-data verification.
- [ ] Add mount-coordinated online scrub against a stable generation/snapshot view.
- [x] Detect deliberate silent single-byte corruption through both normal reads and scrub.
- [x] Prove pre-commit data overwrite crashes retain old bytes and post-checkpoint crashes expose new bytes.
- [x] Verify repeated data-CoW overwrites reclaim superseded data/checksum blocks rather than leaking free space.
- [x] Exercise checksum-object chains across files larger than one checksum object.
