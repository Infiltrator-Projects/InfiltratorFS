<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Format 0.11 Conformance

Format 0.11 includes regular-file hard links, named snapshot roots and retained historical generations. Pre-1.0 development builds accept exactly current Format 0.11 and do not open earlier development formats. Newly formatted volumes enable UTF-8 names, sparse extents, inline data, shared extents, paged metadata, symbolic links, hard links and snapshots.

## Required representation

A conforming implementation must preserve:

- 4096-byte filesystem blocks;
- exact Format 0.11 major/minor identity; all other development revisions are rejected;
- fixed-width integer fields encoded little-endian;
- the exact packed field offsets and sizes declared in `include/infilfs/format.h`;
- three checkpoint locations at block zero, the volume midpoint and the final block;
- CRC64-ECMA checkpoint and metadata checksums;
- SHA-256 logical-data checksums;
- mandatory `INFS_INCOMPAT_UTF8_NAMES` support;
- mandatory `INFS_INCOMPAT_SPARSE_EXTENTS` support;
- `INFS_INCOMPAT_INLINE_DATA` governing inline file-payload interpretation;
- `INFS_INCOMPAT_SHARED_EXTENTS` governing duplicate normal-data ownership;
- `INFS_INCOMPAT_PAGED_METADATA` governing version-2 directory/index heads;
- `INFS_INCOMPAT_SYMBOLIC_LINKS` governing symbolic-link objects;
- `INFS_INCOMPAT_HARD_LINKS` governing multiple regular-file namespace references;
- `INFS_INCOMPAT_SNAPSHOTS` governing snapshot-catalog objects;
- object version 1 for classic objects and version 2 for paged directory/index heads;
- owner-identified, generation-matched, CRC64-protected directory and index pages;
- nonzero 128-bit filesystem and object identities;
- nonzero committed generations;
- a NUL-terminated, well-formed UTF-8 checkpoint label with canonical zero padding;
- zero bytes in currently reserved checksum-field tails, metadata block tails, directory-record padding and currently reserved structure fields;
- strict rejection of overlong UTF-8, surrogate code points, truncated sequences and values above U+10FFFF;
- byte-exact, case-sensitive namespace comparison;
- no stored `.` or `..` directory entries;
- exactly one namespace reference to each non-root directory or symbolic link and one or more references to each regular file;
- exact parent-ID agreement between a namespace entry and its child object;
- root-to-leaf reachability of every indexed file/directory, with the root unreferenced and parentless;
- file link count equal to its exact incoming directory-reference count and directory link count `2 + direct child-directory count`;
- common attributes and isolated POSIX compatibility metadata;
- only currently defined portable attribute flags; future security/xattr object references remain zero until their feature is defined;
- normal extents with nonzero physical locations and hole extents with physical location zero;
- contiguous logical extent coverage through the rounded-up logical file size for extent-backed files;
- zero reads and zero allocated-size contribution for hole ranges;
- SHA-256 entries for every allocated extent-backed data block, addressed by sorted sparse checksum chains;
- every indexed checksum object reachable exactly once from its owning file's checksum head, with matching owner/parent identities and strictly increasing aligned segment starts;
- an allocation bitmap large enough to represent every committed volume block before any bitmap bit is read; and
- exact ownership of every allocated in-volume block by the live graph or a retained snapshot graph, with duplicate normal-data ownership within one generation permitted only for shared extents.

Metadata ownership overlap and unreachable allocated blocks are corruption. Inline bytes do not own extra bitmap blocks because they reside within the already-owned file metadata object. Multiple files may own the same normal data block only when `INFS_INCOMPAT_SHARED_EXTENTS` is enabled.

## Inline-file contract

When `INFS_INCOMPAT_INLINE_DATA` is enabled, a non-empty regular file from 1 through `INFS_INLINE_DATA_MAX` bytes may use the inline representation. A conforming inline file has:

- `extent_count == 0`;
- a zero `checksum_head_id`;
- `data_checksum_type == INFS_CHECKSUM_SHA256`;
- payload size exactly `sizeof(struct infs_file_payload_disk) + sizeof(struct infs_data_checksum_disk) + logical_size`;
- one 32-byte digest immediately after the fixed file payload; and
- exactly `logical_size` inline bytes immediately after that digest.

`INFS_INLINE_DATA_MAX` is 3,840 bytes for the current 4096-byte object layout. The digest is SHA-256 over a 4096-byte logical data block consisting of the inline bytes followed by zeros. The enclosing object's CRC64 independently protects the digest and inline bytes as metadata. Inline files allocate no separate data or checksum blocks and therefore contribute zero data blocks to `allocated_size`.

A non-empty `extent_count == 0` file is corruption when inline data is not enabled for the volume. An inline file larger than `INFS_INLINE_DATA_MAX`, with a nonzero checksum head, wrong payload size or mismatched inline digest is corruption. An empty file remains the fixed file payload with logical size zero, no extents and no checksum head.

Growth beyond the inline threshold must preserve the existing bytes while transactionally publishing an extent-backed replacement. Shrink to the inline threshold or below on an inline-enabled volume may fold an extent-backed file inline and must reclaim its superseded data/checksum blocks under the normal deferred-reclamation rules. Inline sparse gaps and inline hole punching are stored as literal zero bytes while logical size is preserved.

## Shared-extent contract

A reflink creates a distinct file object with its own identity, attributes and checksum chain while reusing the source file's normal physical data blocks. Inline files are copied inline rather than made to share their metadata object. Later writes replace only the changed blocks through CoW. Truncate, hole punching, inline folding, rename replacement and unlink must not reclaim a normal block while any other committed file extent still refers to it. Scrub accepts duplicate normal-data ownership only when the shared-extents feature is present and continues to reject all metadata overlap.

## Paged-metadata contract

When `INFS_INCOMPAT_PAGED_METADATA` is set, the root directory and object-index head use object version 2. Their payloads contain little-endian physical pointers to metadata pages rather than inline entries. Every page has the correct directory/index magic, owner ID, generation, entry count, byte count, reserved-zero field and CRC64. Page pointers must be nonzero, allocated, in range, unique and reachable only from their owning head. Entry totals in the head must equal the combined page totals. A mismatch between the paged feature bit and either required head version is corruption.

## Development format and feature flags

Before the first stable release, a development build accepts only its exact current format major/minor. A later development format replaces the earlier format; no compatibility reader or migration path is required.

Within current Format 0.11, the conventional three feature classes remain defined for the stable compatibility policy:

- unknown `incompat_flags` require refusal to open;
- unknown `ro_compat_flags` may be opened read-only but must not be opened writable;
- unknown `compat_flags` may be ignored because they do not change required read/write interpretation.

`INFS_INCOMPAT_UTF8_NAMES` and `INFS_INCOMPAT_SPARSE_EXTENTS` are required for Format 0.11. The remaining known incompatible bits select representations understood by this format, and the current formatter enables all of them. Format 0.11 defines no compatible or read-only-compatible bits for newly created volumes.

## Symbolic-link acceptance

A symbolic-link object is accepted only when the symbolic-link incompatible feature is enabled. It must use classic object version 1, valid common/POSIX metadata, link count 1, and an exact payload containing a nonempty NUL-free UTF-8 target of at most 3,888 bytes. The target length, payload size and common logical size must agree; the reserved field must be zero. Directory entries, index entries, namespace reachability and scrub all preserve and verify object type 5. Relative and absolute targets are opaque to the portable core and resolved by the operating-system adapter.

## Hard-link acceptance

Hard links are restricted to regular files. All names share one indexed object ID, file payload, extent/checksum chain, attributes and content. The stored link count must equal the exact number of reachable directory entries. A multiply linked file must have a zero parent ID; a singly linked file may retain its containing parent or zero after having previously been multiply linked. Creating a link, unlinking one name and replacing one linked destination update the directory graph and file reference count in the same transaction. Directory and symbolic-link multiple references remain corruption.

## Snapshot acceptance

The snapshot feature permits one classic snapshot-catalog object whose reserved 16-byte ID is `INFS-SNAP-CAT-01` and whose parent ID is zero. Its payload contains zero to 27 fixed-size records. Names are unique, contain 1–63 well-formed UTF-8 bytes, exclude NUL and `/`, and have canonical zero padding. Record flags and reserved fields are zero.

Every record identifies a generation strictly older than its containing generation, a bitmap image with the same geometry as the active volume, exact free-block accounting, a valid object-index root, a valid namespace root and matching root object ID. Snapshot bitmaps own their own bitmap blocks and may include prior snapshot graphs. Recursively referenced catalog generations must strictly decrease.

The active allocation bitmap equals the union of the live graph and all directly catalogued snapshot bitmaps. Snapshot creation captures a stable published generation and commits its immutable bitmap/catalog reference atomically. Ordinary CoW reclamation must preserve every snapshot-owned block. Deletion removes the name and reclaims only blocks absent from both the live graph and every remaining snapshot graph. Snapshot lookup, list, stat, read and readlink operations are read-only.

## Portable result contract

The core and storage-service boundary return `infs_status` values. Successful operations return `INFS_STATUS_OK`; failures return a stable negative value such as `INFS_STATUS_CORRUPT`, `INFS_STATUS_NOT_FOUND`, `INFS_STATUS_NO_MEMORY`, `INFS_STATUS_IO_ERROR` or `INFS_STATUS_NO_SPACE`. Byte-count APIs return a non-negative count or a negative `infs_status`. Internal helpers preserve the concrete originating status unless they have positively identified a more specific filesystem condition.

Operating-system adapters perform native translation. The POSIX adapter maps between `infs_status` and `errno`; the Win32 storage and transfer adapter maps the same core results without changing the disk format.

A writable storage backend must provide positioned write, durable flush, random-byte and current-time services in addition to the read/size services required for read-only access. A mutation fails if its required clock service fails; the filesystem does not silently substitute a zero timestamp. The formatter follows the same fail-closed clock policy for the initial root timestamps.

## Recovery selection

Physical checkpoint copies are first screened independently for checkpoint checksum, format and volume-geometry validity. A read-only opener may tolerate an unreadable physical checkpoint copy as lost redundancy when another independently valid checkpoint copy survives. A writable opener instead returns the originating storage error because an unreadable copy could contain the only newer committed generation and must not be overwritten by healing.

Recovery considers surviving candidates in descending generation order and validates the complete graph referenced by each candidate: bitmap geometry/accounting, critical allocation, root, object index, exact ownership, namespace graph, checksum graph, extent shapes and inline-file payload integrity.

A candidate whose committed referenced graph is structurally corrupt may be rejected in favor of the next older individually valid committed candidate. External failures encountered while validating a candidate graph, such as storage I/O errors, memory exhaustion or unsupported feature/format semantics, are not reclassified as corruption and must not be silently hidden by falling back to an older generation.

A read-only recovery leaves physical checkpoint copies unchanged. After writable recovery selects an older valid graph, the implementation rewrites and durably flushes all three checkpoint replicas to that selected committed generation before returning the volume writable.

Once the first generation-N+1 checkpoint is durably flushed, the transaction is committed. Failure while replicating that already-committed checkpoint to secondary locations does not retroactively turn the namespace or data mutation into a failed operation. If the first checkpoint write or its immediately following flush fails, durability is indeterminate: the live volume disables mutation and requires close-and-reopen recovery before allocation can continue.

The POSIX backend acquires nonblocking advisory locks for the lifetime of each storage opener. Read-only openers use shared locks. Writable openers and the formatter use exclusive locks. Lock contention returns `INFS_STATUS_BUSY` before volume recovery or destructive formatting begins.

Ordinary rename replacement is a single filesystem transaction. A file may replace an existing file; a directory may replace an empty directory. Replacing a non-empty directory is rejected. Type-incompatible replacement is rejected. A trailing slash retains directory semantics for both source and destination. The source object's persistent ID is retained and no intermediate committed namespace is published.

## Automated checks

`infilfs-format-conformance` checks exact structure sizes and offsets, independently constructs expected checkpoint bytes, rejects both earlier and future development formats, rejects unsupported incompatible features and verifies UTF-8/checksum rules. `infilfs-namespace-links` covers symbolic-link targets plus hard-link identity, shared writes, cross-directory rename, replacement, unlink, reference-count corruption, scrub and remount. `infilfs-snapshots` covers immutable names/data, nested retained generations, remount, historical scrub, corruption detection, deletion and final reclamation.

`infilfs-inline-files` constructs a deterministic inline-enabled volume in memory and verifies:

- an empty file begins without external storage;
- small writes remain inline;
- unwritten high-offset inline gaps read as zero;
- inline hole punching preserves surrounding bytes;
- exactly 3,840 bytes remain inline;
- byte 3,841 triggers promotion to an ordinary checksummed extent;
- truncating the promoted file back below the threshold folds it inline and releases external data/checksum storage;
- scrub verifies inline logical data; and
- inline state survives close and read-only reopen.

`infilfs-volume-conformance` verifies the complete in-memory volume graph and sparse-file behavior, including a 1 TiB logical sparse file in a small image, out-of-order sparse checksum segments, zero-filled reads, punching, reclamation, scrub and malformed extent rejection.

`infilfs-reflink` verifies shared physical blocks, independent checksums, CoW separation, safe truncate/unlink reclamation, inline cloning, scrub and remount persistence.

`infilfs-paged-metadata` creates entries beyond the classic one-block limits, exercises lookup/list/rename/remove, verifies deferred buffered publication, scrubs the complete page graph and reopens it read-only.

`infilfs-forensic-scan` creates multiple CoW generations, finds current and orphaned authenticated metadata, proves a damaged stale object is excluded, and erases all three checkpoints to verify raw object discovery continues with unknown allocation state.

`infilfs-hardening-conformance` covers checkpoint-label termination, canonical padding/reserved fields, object versions, read-only-compatible feature semantics, exact block ownership, namespace uniqueness/reachability/parentage, checksum-object reachability, read-status propagation, POSIX rename replacement, post-commit checkpoint-replica failure, explicit replica healing and mandatory reopen after an indeterminate first-checkpoint flush. Its minimal current-format fixtures retain ordinary extent behavior without inline or paged representations.

`infilfs-open-hardening`, `infilfs-063-hardening`, `infilfs-posix-locking` and `infilfs-sparse-files` retain the existing geometry, recovery, locking and crash-atomicity regression gates.

GitHub Actions builds and tests the full Linux implementation, separately builds the portable core and native transfer application with Microsoft Visual C, and opens a Linux-created Format 0.11 image through the Win32 backend. The hardening matrix also runs Clang, AddressSanitizer/UndefinedBehaviorSanitizer and GCC `-fanalyzer`. When `/dev/fuse` is available, `infilfs-mint-fuse` performs mounted copy, rename, permission, symbolic-link, hard-link, persistent xattr, FIFO, Unix-socket, normal-fallocate, sparse, punch, unmount and remount verification.

Implementation 0.16.0 requires named read-only snapshot roots through the portable API and direct-image tool. Conformance verifies immutable historical data and namespace state, nested generation retention, full remount, scrub of retained data, feature gating, deletion and final block reclamation.

Implementation 0.16.1 removes pre-1.0 backward-format acceptance. Conformance requires both earlier and future development format minors to be rejected.

Implementation 0.16.2 requires physically separate data and metadata allocation directions. A sequential file written across 192 separate durable calls must complete, retain no more than two extents on the deterministic test volume, pass full data scrub and remain byte-exact after reopen. This crosses all 161 inline extent slots and reproduces the write-cycle pattern behind the former approximately 40 MiB direct-copy `EFBIG` failure.

Implementation 0.16.3 requires the Linux FUSE adapter to preserve `user.*` extended attributes across adapter operations, create and report FIFO and Unix-domain socket nodes with their correct POSIX types, accept normal `fallocate` without changing pre-existing bytes, return stable inode identity and link counts for hard links, and provide nonzero file-capacity values through `statvfs`. These are Linux adapter semantics stored using hidden system-marked ordinary Format 0.11 objects and do not alter the portable on-disk format.

Implementation 0.16.4 requires Linux-adapter hidden-metadata cleanup to probe for metadata existence before starting a nested unlink. An absent adapter metadata record must not abort or roll back the user object unlink. Mounted conformance verifies that an ordinary unlink is observably absent immediately afterward and that recursive `rm -rf` can remove a nested ordinary tree completely.

Implementation 0.15.0 requires regular-file hard links through both the portable API and Linux FUSE. Mounted conformance verifies shared inode identity, reference count, byte identity and remount persistence. Offline conformance additionally verifies write-through coherence, cross-directory rename, unlink survival, atomic replacement of one linked destination and rejection of a validly checksummed but incorrect reference count.

Implementation 0.14.0 requires native relative and absolute symbolic links through both the portable API and Linux FUSE. Mounted conformance verifies `ln -s`, `readlink`, target traversal and persistence across remount. Offline conformance verifies exact target bytes and metadata, namespace mutations, scrub, feature gating and invalid target rejection.

Implementation 0.12.0 additionally requires mounted conformance for descriptor lifetime across direct rename, parent-directory rename, unlink and atomic replacement of an open destination. The test continues reading, writing, truncating and syncing through the original descriptor after its pathname changes or disappears. Retained objects use an adapter-owned hidden, system-marked directory built from ordinary Format 0.8 namespace objects; final close and the next writable mount both verify reclamation without a disk-format extension.

Implementation 0.13.0 requires the installed `mount.infiltratorfs` helper to preserve source and mountpoint arguments exactly, translate `-r`, `-w` and `-o` options deterministically, ignore only mount-owned verbose/no-mtab flags and reject unknown options. `fsck.infiltratorfs` is a complete read-only scrub front end: clean media returns 0, detected but uncorrected corruption returns 4, operational failure returns 8 and invalid invocation returns 16. Utility conformance verifies paths containing spaces, option forwarding and all status mappings.

The high-level FUSE adapter deliberately uses libfuse's documented offsetless `readdir` mode: it ignores the incoming directory offset and passes zero offsets to the filler so libfuse consumes the complete directory in one operation. Synthetic resumable cookies are not part of the current adapter contract.

## Compatibility rule

Until the first stable release, a change that alters golden checkpoint bytes, packed offsets or interpretation increments the development format and replaces its predecessor without a compatibility reader or migration requirement. From the first stable release onward, such changes require a compatible feature extension or a defined migration policy. Tightening rejection of metadata already defined as reserved, unreachable, multiply owned, structurally inconsistent or geometrically impossible remains implementation hardening.


Implementation 0.16.5 permits transaction-private full-overwrite allocations to skip pre-zeroing only when the complete data or bitmap image is written before checkpoint publication. Mounted conformance verifies an aligned sequential write through FUSE, explicit sync, and byte-identical readback. Linux automatic publication may use a bounded volume-adaptive threshold; explicit fsync/sync remains immediate. Format 0.11 is unchanged.
