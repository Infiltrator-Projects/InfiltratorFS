<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Format 0.8 Conformance

Format 0.8 retains Format 0.7 inline files and feature-gated shared extents while adding version-2 directory/index heads and independently checksummed metadata pages. Format 0.6 and 0.7 volumes remain readable. New Format 0.8 volumes enable UTF-8 names, sparse extents, inline data, shared extents and paged metadata.

## Required representation

A conforming implementation must preserve:

- 4096-byte filesystem blocks;
- fixed-width integer fields encoded little-endian;
- the exact packed field offsets and sizes declared in `include/infilfs/format.h`;
- three checkpoint locations at block zero, the volume midpoint and the final block;
- CRC64-ECMA checkpoint and metadata checksums;
- SHA-256 logical-data checksums;
- mandatory `INFS_INCOMPAT_UTF8_NAMES` support;
- mandatory `INFS_INCOMPAT_SPARSE_EXTENTS` support for Format 0.6 and later;
- `INFS_INCOMPAT_INLINE_DATA` accepted only for Format 0.7 and later;
- `INFS_INCOMPAT_SHARED_EXTENTS` governing duplicate normal-data ownership;
- mandatory `INFS_INCOMPAT_PAGED_METADATA` for newly formatted Format 0.8 volumes;
- object version 1 for classic objects and version 2 for paged directory/index heads;
- owner-identified, generation-matched, CRC64-protected directory and index pages;
- nonzero 128-bit filesystem and object identities;
- nonzero committed generations;
- a NUL-terminated, well-formed UTF-8 checkpoint label with canonical zero padding;
- zero bytes in currently reserved checksum-field tails, metadata block tails, directory-record padding and currently reserved structure fields;
- strict rejection of overlong UTF-8, surrogate code points, truncated sequences and values above U+10FFFF;
- byte-exact, case-sensitive namespace comparison;
- no stored `.` or `..` directory entries;
- exactly one namespace reference to each non-root file/directory because hard links are not yet defined;
- exact parent-ID agreement between a namespace entry and its child object;
- root-to-leaf reachability of every indexed file/directory, with the root unreferenced and parentless;
- file link count 1 and directory link count `2 + direct child-directory count`;
- common attributes and isolated POSIX compatibility metadata;
- only currently defined portable attribute flags; future security/xattr object references remain zero until their feature is defined;
- normal extents with nonzero physical locations and hole extents with physical location zero;
- contiguous logical extent coverage through the rounded-up logical file size for extent-backed files;
- zero reads and zero allocated-size contribution for hole ranges;
- SHA-256 entries for every allocated extent-backed data block, addressed by sorted sparse checksum chains;
- every indexed checksum object reachable exactly once from its owning file's checksum head, with matching owner/parent identities and strictly increasing aligned segment starts;
- an allocation bitmap large enough to represent every committed volume block before any bitmap bit is read; and
- exact ownership of every allocated in-volume block by a checkpoint, the current bitmap, the current object-index head/page, a directory page, an indexed metadata object or a normal data extent, with duplicate normal-data ownership permitted only for shared extents.

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

## Feature-flag compatibility

Format 0.8 uses the conventional three feature classes deliberately:

- unknown `incompat_flags` require refusal to open;
- unknown `ro_compat_flags` may be opened read-only but must not be opened writable;
- unknown `compat_flags` may be ignored because they do not change required read/write interpretation.

`INFS_INCOMPAT_UTF8_NAMES`, `INFS_INCOMPAT_SPARSE_EXTENTS` and `INFS_INCOMPAT_PAGED_METADATA` are required on newly formatted Format 0.8 volumes. `INFS_INCOMPAT_INLINE_DATA` changes file-payload interpretation and is invalid before Format 0.7. `INFS_INCOMPAT_SHARED_EXTENTS` changes normal-data ownership rules. `INFS_INCOMPAT_PAGED_METADATA` is invalid before Format 0.8. Format 0.8 defines no compatible or read-only-compatible bits.

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

`infilfs-format-conformance` checks exact structure sizes and offsets, independently constructs expected Format 0.8 checkpoint bytes, proves version-gated features cannot appear under an older minor version, accepts compatible legacy checkpoints, rejects unsupported incompatible features and verifies UTF-8/checksum rules.

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

`infilfs-hardening-conformance` covers checkpoint-label termination, canonical padding/reserved fields, object versions, read-only-compatible feature semantics, exact block ownership, namespace uniqueness/reachability/parentage, checksum-object reachability, read-status propagation, POSIX rename replacement, post-commit checkpoint-replica failure, explicit replica healing and mandatory reopen after an indeterminate first-checkpoint flush. Its legacy fixtures retain ordinary extent behavior without inline or paged representations.

`infilfs-open-hardening`, `infilfs-063-hardening`, `infilfs-posix-locking` and `infilfs-sparse-files` retain the existing geometry, recovery, locking and crash-atomicity regression gates.

GitHub Actions builds and tests the full Linux implementation, separately builds the portable core and native transfer application with Microsoft Visual C, and opens a Linux-created Format 0.8 image through the Win32 backend. The hardening matrix also runs Clang, AddressSanitizer/UndefinedBehaviorSanitizer and GCC `-fanalyzer`. When `/dev/fuse` is available, `infilfs-mint-fuse` performs mounted copy, rename, permission, sparse, punch, unmount and remount verification.

The high-level FUSE adapter deliberately uses libfuse's documented offsetless `readdir` mode: it ignores the incoming directory offset and passes zero offsets to the filler so libfuse consumes the complete directory in one operation. Synthetic resumable cookies are not part of the current adapter contract.

## Compatibility rule

A future change that alters golden checkpoint bytes, packed offsets, extent or inline semantics, namespace representation, or the interpretation required by existing compatibility feature bits requires an explicit format revision or a newly defined compatibility feature bit as appropriate. Tightening rejection of metadata already defined as reserved, unreachable, multiply owned, structurally inconsistent or geometrically impossible is implementation hardening and does not by itself create a new disk format.
