<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Architecture

## 1. Identity and scope

InfiltratorFS is a clean-sheet, platform-neutral local filesystem. Linux is the primary mounted development host; Windows has a native storage and transfer adapter but not yet an Explorer filesystem driver. Operating-system APIs are adapters around the filesystem rather than properties of its disk format.

The same formatted volume must be usable by Linux and Windows implementations without translating the filesystem or changing its on-disk identity.

The design assumes that storage can return incorrect data, power loss is ordinary, critical metadata must not depend on one root structure, and allocation policy should adapt to both media and workload.

## 2. Layering rule

InfiltratorFS is divided into four layers:

1. **On-disk format** — fixed-width little-endian records, object identities, allocation, transactions and integrity.
2. **Portable core engine** — namespace, inline data, extents, checksums, CoW and recovery logic written in C17.
3. **Platform services** — callbacks for positioned read/write, durable flush, target size, secure randomness, wall-clock time and close.
4. **Operating-system adapters** — Linux FUSE/POSIX plus Win32 image/raw-partition access today; native Linux and Windows filesystem drivers later.

The core does not store or expose a Linux file descriptor. It does not depend on FUSE, a Linux VFS inode, `struct stat`, `off_t`, UID/GID types, `errno` or a Windows handle. Adapters translate those concepts at the boundary. Core and storage operations use stable negative `infs_status` values; byte-count APIs return either a non-negative count or one of those values.

The canonical core path form currently uses UTF-8 components separated by `/`. Separators are not stored in directory entries. A Windows adapter may accept Windows path syntax and translate it into core components.

The Linux FUSE adapter assigns each opened file a stable in-memory handle containing its persistent object ID and current retained path. Rename updates matching handle paths, including descendants of a renamed directory. Unlink and atomic replacement move an open object into an adapter-owned hidden, system-marked directory until its final descriptor closes. Writable mount startup reclaims stale retention directories left by interruption. These are ordinary transactional Format 0.8 namespace operations, so the lifetime policy adds no adapter state to the disk format.

Linux standard-utility integration remains outside the portable core. `mount.infiltratorfs` adapts the util-linux mount-helper contract to the FUSE process, and `fsck.infiltratorfs` maps the portable read-only scrub result to standard fsck status bits. Neither helper introduces persistent state or a second filesystem implementation.

## 3. Persistent object model

Files, directories, checksum records and future metadata classes are persistent objects identified by 128-bit IDs. A pathname is a namespace mapping, not object identity.

Directories map UTF-8 names to object IDs. Renaming or physically relocating an object does not change its ID. Linux may derive a stable inode number from the ID; Windows may expose the full value as a file ID. Neither projection is stored as the canonical identity.

Format 0.8 uses checksummed metadata pages for the persistent object index and directories. The index head stores physical pointers to index pages; directory heads store pointers to pages of variable-length entries. The root object block is also present in each checkpoint as a bootstrap and recovery anchor. These are bounded one-level page arrays rather than the future generation-aware trees.

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

Format 0.8 names are well-formed UTF-8 and are limited to 255 bytes per component. NUL and `/` are invalid. `.` and `..` are adapter/core navigation syntax and are not stored.

Format 0.9 adds first-class symbolic-link namespace objects behind an incompatible feature bit. Each link retains common object identity, timestamps and portable/POSIX attributes while storing a nonempty inline UTF-8 target of up to 3,888 bytes. The portable core creates, reads and validates the opaque target; Linux FUSE delegates normal relative/absolute traversal to the kernel through `readlink`.

Format 0.10 adds regular-file hard links behind a separate incompatible feature bit. Directory entries may share one file object ID, data representation and attribute set; the stored file link count must equal the exact incoming namespace-reference count. The second name clears the file's single-parent hint, while directories and symbolic links retain strict single-parent ownership. Link, unlink and replacement accounting are part of the same CoW transaction as their directory changes.

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

A crash before the first new checkpoint retains generation `N`. A crash after it exposes generation `N+1`. Mixed readable checkpoint generations are healed before a writable opener allocates again. If a physical checkpoint cannot be read, writable recovery fails closed because that location may contain the only newer commit; read-only inspection may still use surviving replicas. If the live writer cannot establish whether first-checkpoint publication became durable, it disables further mutation and requires checkpoint recovery through close and reopen. No journal replay is required.

Inline-data updates follow the same transaction model as other metadata updates: the replacement file object is unreachable until the new object index and checkpoint generation are published. Promotion from inline data to extents and folding from extents back inline occur inside one transaction, so applications see either the old complete representation or the new complete representation.

The POSIX adapter places a shared advisory lock on every read-only storage opener and an exclusive advisory lock on writers. The formatter participates in the same protocol. This makes the prototype's single-writer assumption an enforced process boundary rather than a caller convention.

The required operation is a durable flush/barrier supplied by the storage adapter. Linux currently implements it with `fsync`; a Windows implementation can use the corresponding Windows storage primitive.

## 7. Allocation

One bitmap bit describes one 4096-byte block. The bitmap is the authoritative free-space record. A future free-extent tree is only a rebuildable accelerator.

Extent-backed file data uses normal extents, which map logical blocks to physical storage, and hole extents, which map logical ranges to zeros without allocating data blocks. Truncate growth creates holes, writes allocate only touched blocks, and full-block hole punching reclaims storage while preserving logical size.

Format 0.8 retains inline storage for non-empty files up to 3,840 bytes inside their existing file metadata object. Inline data consumes no separate data/checksum blocks. Crossing the threshold promotes the file to ordinary extents; shrinking back to the threshold or below can fold it inline and reclaim the external blocks. Inline storage is deliberately a file representation, not a new extent type.

Format 0.8 permits shared normal extents. A reflink creates a new file object and checksum chain that refer to the source data blocks; later writes, truncate, hole punching, rename replacement and unlink preserve shared blocks until their final reference disappears. Compression, integrity placement and other future data states require explicitly versioned representations. The allocator does not yet classify archive, sequential-growing, streaming, random-write, temporary or VM/disk-image workloads.

## 8. Integrity and recovery

Metadata blocks are self-identifying, versioned and checksummed. Format 0.11 uses CRC64-ECMA for object heads and metadata pages and SHA-256 for logical file data.

Every allocated extent-backed logical data block has a separate checksum entry stored in checksummed CoW metadata. Hole extents need no data checksum. Checksum objects form a sorted sparse chain, so a high-offset write allocates checksum metadata for its own segment rather than every preceding hole.

Inline data has no external checksum object. Its file object stores one SHA-256 digest immediately before the inline bytes. The digest authenticates the inline bytes followed by zeros to a complete 4096-byte logical block, while the object's CRC64 independently authenticates the full metadata block including that digest and the inline data itself.

Normal reads verify data before returning it. `infilfs-scrub` verifies every live and snapshot-retained regular-file block and each non-empty inline logical data block. Repair is deferred until trustworthy redundant data placement exists.

The portable forensic scanner reads raw storage one complete block at a time and independently authenticates recognizable checkpoints, object heads, directory pages and index pages. When a valid current checkpoint and bitmap survive, it distinguishes current allocation from stale checkpoints and orphaned CoW metadata; if no checkpoint survives, it continues discovery with allocation state reported as unknown. It never modifies the target or treats discovered historical metadata as authoritative live state. Data-block reconstruction and automated repair remain future work.

## 9. Reflinks, snapshots and history

Reflinks and shared extents are implemented. Shared-block lifetime is reconstructed from the committed object graph and remains transactional across CoW replacement and reclamation.

Format 0.11 adds a feature-gated snapshot-catalog object. Each named record captures an immutable generation number, root object, object-index root and dedicated bitmap image. The live allocation bitmap is the union of the active graph and every retained snapshot graph. CoW superseded blocks therefore remain unavailable while any snapshot references them, and deleting the last relevant snapshot reclaims them transactionally.

Snapshots are read-only and use the ordinary portable lookup, attribute, directory, symlink and file-read paths through an isolated historical view. Catalog names are unique, well-formed UTF-8 and limited to 63 bytes; the current bounded catalog stores at most 27 records. Rollback and native undelete policy remain future work.

## 10. Media, protection, compression and encryption

The disk format remains common while allocation adapts to rotational, solid-state, removable-flash and zoned media.

Protection classes will allow checksum-only, ordinary, duplicated and strongest-available policies. Duplication on one physical device must never be represented as protection from complete device loss.

Compression operates per extent. Encryption operates at object or encryption-domain level with key wrapping separate from data encryption.

## 11. Non-goals

InfiltratorFS does not use FAT-style linked allocation, a fixed global inode table, a single irreplaceable superblock, unchecked critical metadata or global synchronous deduplication. It does not depend on FUSE, Linux, Windows or another filesystem's disk structures.
