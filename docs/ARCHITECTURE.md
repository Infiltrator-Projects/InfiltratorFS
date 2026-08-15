<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Architecture

## 1. Identity and scope

InfiltratorFS is a clean-sheet, platform-neutral local filesystem. Linux is the first development and test host, while Windows is an intended future host. Operating-system APIs are adapters around the filesystem rather than properties of its disk format.

The same formatted volume must be usable by Linux and Windows implementations without translating the filesystem or changing its on-disk identity.

The design assumes that storage can return incorrect data, power loss is ordinary, critical metadata must not depend on one root structure, and allocation policy should adapt to both media and workload.

## 2. Layering rule

InfiltratorFS is divided into four layers:

1. **On-disk format** — fixed-width little-endian records, object identities, allocation, transactions and integrity.
2. **Portable core engine** — namespace, extents, checksums, CoW and recovery logic written in C17.
3. **Platform services** — callbacks for positioned read/write, durable flush, target size, secure randomness, wall-clock time and close.
4. **Operating-system adapters** — Linux FUSE/POSIX today; native Linux and Windows drivers later.

The core does not store or expose a Linux file descriptor. It does not depend on FUSE, a Linux VFS inode, `struct stat`, `off_t`, UID/GID types, `errno` or a Windows handle. Adapters translate those concepts at the boundary. Core and storage operations use stable negative `infs_status` values; byte-count APIs return either a non-negative count or one of those values.

The canonical core path form currently uses UTF-8 components separated by `/`. Separators are not stored in directory entries. A Windows adapter may accept Windows path syntax and translate it into core components.

## 3. Persistent object model

Files, directories, checksum records and future metadata classes are persistent objects identified by 128-bit IDs. A pathname is a namespace mapping, not object identity.

Directories map UTF-8 names to object IDs. Renaming or physically relocating an object does not change its ID. Linux may derive a stable inode number from the ID; Windows may expose the full value as a file ID. Neither projection is stored as the canonical identity.

Format 0.6 uses a persistent object index mapping object IDs to current metadata blocks. The root object block is also present in each checkpoint as a bootstrap and recovery anchor. The current one-block index and directories are prototype limits.

## 4. Platform-neutral attributes

Common object attributes contain:

- logical size;
- link count;
- portable flags;
- birth time;
- access time;
- content-modification time;
- metadata-change time;
- a future security-object ID;
- a future extended-attributes-object ID.

Times are signed nanoseconds since the Unix epoch. That is a disk encoding, not a dependency on Unix; Windows adapters convert to and from `FILETIME`.

Read-only, hidden, system, archive, temporary and not-content-indexed flags have common definitions. Additional platform flags can be stored through versioned extended attributes.

POSIX permission bits and numeric UID/GID values are retained in a separate compatibility record so the Linux adapter can preserve its expected behaviour. They are not the filesystem's fundamental security model. Future security objects will support typed principals and access-control entries suitable for SID-based Windows security and POSIX mapping.

## 5. Namespace rules

Format 0.6 names are well-formed UTF-8 and are limited to 255 bytes per component. NUL and `/` are invalid. `.` and `..` are adapter/core navigation syntax and are not stored.

Current directory lookup is case-sensitive and compares encoded UTF-8 bytes exactly. Unicode normalization and case-folded directory policies must be explicitly versioned before they are enabled. An adapter must not silently create a second definition of filename equality.

Platform-specific filename restrictions belong in the relevant adapter. A future portability profile may impose a stricter shared subset for removable volumes intended to move routinely between operating systems.

## 6. Transaction model

Committed critical metadata is never overwritten as its only valid copy. A transaction based on generation `N`:

1. snapshots the committed allocation state in memory;
2. reserves replacement blocks without changing a durable root;
3. writes new or changed metadata and data to unreachable blocks;
4. updates the object index through copy-on-write;
5. constructs a new authoritative bitmap;
6. requests a durable storage flush;
7. writes and flushes the new bitmap;
8. publishes one generation-`N+1` checkpoint as the atomic commit point;
9. replicates that checkpoint to the other physical locations;
10. makes superseded generation-`N` blocks reusable only after publication.

A crash before the first new checkpoint retains generation `N`. A crash after it exposes generation `N+1`. Mixed checkpoint generations are healed before a writable opener allocates again. No journal replay is required.

The required operation is a durable flush/barrier supplied by the storage adapter. Linux currently implements it with `fsync`; a Windows implementation can use the corresponding Windows storage primitive.

## 7. Allocation

One bitmap bit describes one 4096-byte block. The bitmap is the authoritative free-space record. A future free-extent tree is only a rebuildable accelerator.

File data is stored in extents rather than linked cluster chains. Format 0.6 distinguishes normal extents, which map logical blocks to physical storage, from hole extents, which map a logical range to zeros without allocating data blocks. Truncate growth creates holes, writes allocate only touched blocks, and full-block hole punching reclaims storage while preserving logical size. Compression, shared, inline, integrity and placement states remain future extent types.

The allocator will eventually distinguish archive, sequential-growing, streaming, random-write, temporary and VM/disk-image workloads. Policy may alter extent size, locality, CoW, compression, aggregation and protection without changing ordinary file semantics.

## 8. Integrity and recovery

Metadata blocks are self-identifying, versioned and checksummed. Format 0.6 retains CRC64-ECMA for metadata and SHA-256 for complete 4096-byte file-data blocks. Every allocated logical data block has a separate checksum entry stored in checksummed CoW metadata. Hole extents need no data checksum. Checksum objects form a sorted sparse chain, so a high-offset write allocates checksum metadata for its own segment rather than every preceding hole.

Normal reads verify data before returning it. `infilfs-scrub` verifies every currently allocated regular-file block. Repair is deferred until trustworthy redundant data placement exists.

Future forensic tooling will scan raw storage for recognizable checkpoint, object, directory, extent and checksum records. Redundant hints may aid reconstruction, but one representation remains authoritative during normal operation.

## 9. Snapshots, reflinks and history

A snapshot is a retained committed root generation. Two objects may later share immutable extents; writes then replace only the changed ranges. Reference accounting must remain transactional and recoverable.

Retained generations naturally support snapshot, rollback and undelete policies without relying on raw-sector guessing.

## 10. Media, protection, compression and encryption

The disk format remains common while allocation adapts to rotational, solid-state, removable-flash and zoned media.

Protection classes will allow checksum-only, ordinary, duplicated and strongest-available policies. Duplication on one physical device must never be represented as protection from complete device loss.

Compression operates per extent. Encryption operates at object or encryption-domain level with key wrapping separate from data encryption.

## 11. Non-goals

InfiltratorFS does not use FAT-style linked allocation, a fixed global inode table, a single irreplaceable superblock, unchecked critical metadata or global synchronous deduplication. It does not depend on FUSE, Linux, Windows or another filesystem's disk structures.
