<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Format 0.7 Conformance

Implementation 0.7.0 defines the portable contract of on-disk Format 0.7. Format 0.7 preserves the existing packed structure sizes and offsets from Format 0.6 while defining the new `INFS_INCOMPAT_INLINE_DATA` feature and the corresponding inline regular-file payload shape. Format 0.6 volumes remain readable. A Format 0.7 volume may be extent-only when the inline-data bit is clear; new volumes formatted by implementation 0.7.0 enable inline data by default.

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
- metadata object version 1 for current object classes;
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
- exact ownership of every allocated in-volume block by a checkpoint, the current bitmap, the current object-index root, an indexed metadata object or a normal data extent.

Physical ownership overlap and unreachable allocated blocks are corruption. Inline bytes do not own extra bitmap blocks because they reside within the already-owned file metadata object.

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

## Feature-flag compatibility

Format 0.7 uses the conventional three feature classes deliberately:

- unknown `incompat_flags` require refusal to open;
- unknown `ro_compat_flags` may be opened read-only but must not be opened writable;
- unknown `compat_flags` may be ignored because they do not change required read/write interpretation.

`INFS_INCOMPAT_UTF8_NAMES` and `INFS_INCOMPAT_SPARSE_EXTENTS` are required by Format 0.7. `INFS_INCOMPAT_INLINE_DATA` is optional for a specific Format 0.7 volume but, when present, changes required file-payload interpretation and therefore is incompatible. It is invalid on a pre-0.7 checkpoint. Implementation 0.7.0 defines no compatible or read-only-compatible feature bits.

## Portable result contract

The core and storage-service boundary return `infs_status` values. Successful operations return `INFS_STATUS_OK`; failures return a stable negative value such as `INFS_STATUS_CORRUPT`, `INFS_STATUS_NOT_FOUND`, `INFS_STATUS_NO_MEMORY`, `INFS_STATUS_IO_ERROR` or `INFS_STATUS_NO_SPACE`. Byte-count APIs return a non-negative count or a negative `infs_status`. Internal helpers preserve the concrete originating status unless they have positively identified a more specific filesystem condition.

Operating-system adapters perform native translation. The current POSIX adapter maps between `infs_status` and `errno`. A future Windows adapter can map the same values to Win32 errors or NTSTATUS without changing the core or disk format.

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

`infilfs-format-conformance` checks exact structure sizes and offsets, independently constructs expected Format 0.7 checkpoint bytes, proves the inline feature cannot appear under an older minor version, accepts an extent-only Format 0.7 checkpoint, rejects unsupported incompatible features and verifies UTF-8/checksum rules.

`infilfs-inline-files` constructs a deterministic Format 0.7 volume in memory and verifies:

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

`infilfs-hardening-conformance` covers checkpoint-label termination, canonical padding/reserved fields, object versions, read-only-compatible feature semantics, exact block ownership, namespace uniqueness/reachability/parentage, checksum-object reachability, read-status propagation, POSIX rename replacement, post-commit checkpoint-replica failure, explicit replica healing and mandatory reopen after an indeterminate first-checkpoint flush. Its extent-only fixtures also verify that a Format 0.7 volume with the inline feature bit clear retains ordinary extent behavior.

`infilfs-open-hardening`, `infilfs-063-hardening`, `infilfs-posix-locking` and `infilfs-sparse-files` retain the existing geometry, recovery, locking and crash-atomicity regression gates.

GitHub Actions builds and tests the full Linux implementation, including the FUSE adapter, and separately builds the portable core with Microsoft Visual C on Windows. The hardening matrix also runs Clang, AddressSanitizer/UndefinedBehaviorSanitizer and GCC `-fanalyzer`.

The high-level FUSE adapter deliberately uses libfuse's documented offsetless `readdir` mode: it ignores the incoming directory offset and passes zero offsets to the filler so libfuse consumes the complete directory in one operation. Synthetic resumable cookies are not part of the current adapter contract.

## Compatibility rule

A future change that alters golden checkpoint bytes, packed offsets, extent or inline semantics, namespace representation, or the interpretation required by existing compatibility feature bits requires an explicit format revision or a newly defined compatibility feature bit as appropriate. Tightening rejection of metadata already defined as reserved, unreachable, multiply owned, structurally inconsistent or geometrically impossible is implementation hardening and does not by itself create a new disk format.

