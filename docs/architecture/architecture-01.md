<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Architecture — Part 1

## 1. Purpose

InfiltratorFS is a clean-sheet Linux filesystem intended to explore what a general-purpose filesystem should look like when designed in 2026 without legacy on-disk compatibility constraints.

The design is guided by four assumptions:

1. storage can return incorrect data;
2. power loss and interrupted writes are ordinary failure modes;
3. filesystem metadata should be recoverable without trusting a single root structure;
4. placement, protection and write policy should reflect how data is actually used.

## 2. Fundamental model

The filesystem is object-oriented internally. Files, directories, symlinks, snapshots and future metadata classes are persistent objects identified by 128-bit object IDs.

A pathname is a namespace mapping, not the identity of an object. A directory maps names to object IDs. Renaming a file therefore changes namespace metadata without changing the file object's identity. The Linux-facing inode number is a stable 64-bit projection of the persistent 128-bit object ID, not a physical metadata block number, so copy-on-write relocation does not change the object's VFS identity.

Format 0.4 uses a persistent object index mapping object IDs to physical metadata blocks. Directory entries contain object IDs, not physical locations. The root object's current block is also recorded directly in the superblock as a bootstrap/recovery anchor. The single-block format-0.4 index is intentionally temporary; later revisions replace it with a scalable generation-aware tree.

## 3. Transaction model

Format 0.4 retains the transactional metadata model from 0.3 and extends it to ordinary file-data overwrites. Critical committed metadata is never overwritten as its only valid copy. A transaction:

1. starts from committed generation N and snapshots the committed bitmap/superblock in memory;
2. allocates replacement metadata blocks without changing any durable root;
3. copy-on-write updates the object index whenever an object's physical block changes;
4. defers reclamation of blocks belonging to generation N;
5. allocates and writes a new bitmap image describing generation N+1;
6. flushes new metadata/data and then the new bitmap;
7. publishes one checksummed checkpoint as the atomic commit point;
8. replicates the same checkpoint to the other physical checkpoint locations;
9. allows blocks superseded by N+1 to be reused only after that publication.

A crash before the first new checkpoint leaves only generation N reachable. A crash after the first new checkpoint makes N+1 reachable. If the crash occurs while the remaining checkpoint copies are still old, a writable open validates the newest generation and heals all checkpoint copies before performing another allocation. No journal replay is required.

Committed file-data blocks are also copy-on-write in format 0.4. A write constructs replacement data blocks and matching checksum metadata before checkpoint publication, so a pre-commit crash retains the old bytes and a post-commit crash exposes the new bytes. Newly allocated blocks may be filled in place while still unreachable from generation N.

## 4. Checkpoints

The filesystem keeps multiple superblock/checkpoint copies at physically separated positions. Formats 0.1 through 0.4 use three fixed physical copies:

- block 0;
- midpoint block;
- final block.

Formats 0.3 and 0.4 rotate which physical copy receives the first N+1 publication, spreading the commit-point write among the three locations. Later revisions may increase the number of checkpoints. Each checkpoint contains a generation number, format version, UUID, root pointer, allocation-map location, feature flags and checksum.
