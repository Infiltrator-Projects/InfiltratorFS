<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Format 0.6 Conformance

Implementation 0.6.3 defines the hardened portable contract of on-disk Format 0.6. Format 0.6 is intentionally incompatible with Format 0.5 because hole extents and sparse checksum chains change required interpretation and validation. Implementations 0.6.1 through 0.6.3 do not change any packed field, offset, feature bit or disk-layout version introduced by 0.6.0; 0.6.3 adds recovery selection and remaining API/tool hardening without changing the byte format.

## Required representation

A conforming implementation must preserve:

- 4096-byte filesystem blocks;
- fixed-width integer fields encoded little-endian;
- the exact packed field offsets and sizes declared in `include/infilfs/format.h`;
- three checkpoint locations at block zero, the volume midpoint and the final block;
- CRC64-ECMA checkpoint and metadata checksums;
- SHA-256 full-block data checksums;
- mandatory `INFS_INCOMPAT_UTF8_NAMES` support;
- mandatory `INFS_INCOMPAT_SPARSE_EXTENTS` support;
- metadata object version 1 for all Format 0.6 object classes;
- nonzero 128-bit filesystem and object identities;
- nonzero committed generations;
- a NUL-terminated, well-formed UTF-8 checkpoint label with canonical zero padding;
- zero bytes in currently reserved checksum-field tails, metadata block tails, directory-record padding and currently reserved structure fields;
- strict rejection of overlong UTF-8, surrogate code points, truncated sequences and values above U+10FFFF;
- byte-exact, case-sensitive namespace comparison for Format 0.6;
- no stored `.` or `..` directory entries;
- exactly one namespace reference to each non-root file/directory because Format 0.6 has no hard links;
- exact parent-ID agreement between a namespace entry and its child object;
- root-to-leaf reachability of every indexed file/directory, with the root unreferenced and parentless;
- file link count 1 and directory link count `2 + direct child-directory count`;
- common attributes and isolated POSIX compatibility metadata;
- only currently defined portable attribute flags; future security/xattr object references remain zero until their feature is defined;
- normal extents with nonzero physical locations and hole extents with physical location zero;
- contiguous logical extent coverage through the rounded-up logical file size;
- zero reads and zero allocated-size contribution for holes;
- SHA-256 entries for every allocated data block, addressed by sorted sparse checksum chains; inactive slots or checksum objects may remain while the same file still owns other allocated blocks;
- every indexed checksum object reachable exactly once from its owning file's checksum head, with matching owner/parent identities and strictly increasing aligned segment starts;
- an allocation bitmap large enough to represent every committed volume block before any bitmap bit is read;
- exact ownership of every allocated in-volume block by a checkpoint, the current bitmap, the current object-index root, an indexed metadata object or a normal data extent. Physical ownership overlap and unreachable allocated blocks are corruption in Format 0.6.

## Feature-flag compatibility

Format 0.6 uses the conventional three feature classes deliberately:

- unknown `incompat_flags` require refusal to open;
- unknown `ro_compat_flags` may be opened read-only but must not be opened writable;
- unknown `compat_flags` may be ignored because they do not change required read/write interpretation.

Current implementation 0.6.3 defines no compatible or read-only-compatible feature bits. Both masks therefore remain zero for newly formatted volumes, while the reader preserves the compatibility semantics above for future extensions.

## Portable result contract

The core and storage-service boundary return `infs_status` values. Successful operations return `INFS_STATUS_OK`; failures return a stable negative value such as `INFS_STATUS_CORRUPT`, `INFS_STATUS_NOT_FOUND`, `INFS_STATUS_NO_MEMORY`, `INFS_STATUS_IO_ERROR` or `INFS_STATUS_NO_SPACE`. Byte-count APIs return a non-negative count or a negative `infs_status`. Internal helpers must preserve the concrete originating status unless they have positively identified a more specific filesystem condition; they must not collapse unrelated storage, allocation, corruption or memory failures into a generic error.

Operating-system adapters perform native translation. The current POSIX adapter maps between `infs_status` and `errno`. A future Windows adapter can map the same values to Win32 errors or NTSTATUS without changing the core or disk format.

A writable storage backend must provide positioned write, durable flush, random-byte and current-time services in addition to the read/size services required for read-only access. A mutation fails if its required clock service fails; the filesystem does not silently substitute a zero timestamp. The formatter follows the same fail-closed clock policy for the initial root timestamps.

## Recovery selection

Physical checkpoint copies are first screened independently for checkpoint checksum, format and volume-geometry validity. Recovery then considers the surviving candidates in descending generation order and validates the complete graph referenced by each candidate: bitmap geometry/accounting, critical allocation, root, object index, exact ownership, namespace graph and checksum graph.

A candidate whose committed referenced graph is structurally corrupt may be rejected in favor of the next older individually valid committed candidate. Missing or cyclic internal metadata references encountered while validating an otherwise committed graph are classified as graph corruption. External failures such as storage I/O errors, memory exhaustion or unsupported feature/format semantics are not reclassified as corruption and must not be silently hidden by falling back to an older generation.

A read-only recovery leaves physical checkpoint copies unchanged. After writable recovery selects an older valid graph, the implementation rewrites and durably flushes all three checkpoint replicas to that selected committed generation before returning the volume writable.

Once the first generation-N+1 checkpoint is durably flushed, the transaction is committed. Failure while replicating that already-committed checkpoint to secondary locations does not retroactively turn the namespace or data mutation into a failed operation. The volume records degraded checkpoint redundancy and a subsequent explicit sync heals the replicas or reports the storage error.

Ordinary rename replacement is a single filesystem transaction. A file may replace an existing file; a directory may replace an empty directory. Replacing a non-empty directory is rejected. Type-incompatible replacement is rejected. A trailing slash retains directory semantics for both source and destination rather than being silently stripped. The source object's persistent ID is retained and no intermediate committed namespace is published.

## Automated checks

`infilfs-format-conformance` checks exact structure sizes and offsets, independently constructs the expected checkpoint bytes, and rejects incompatible versions, altered geometry, unsupported incompatible features, checksum damage and malformed UTF-8.

`infilfs-volume-conformance` constructs a deterministic complete volume in memory. It verifies the valid image, checkpoint redundancy and rejection of:

- three damaged checkpoints;
- unknown incompatible features;
- incorrect free-space accounting;
- a critical block marked free;
- a mismatched root identity;
- malformed UTF-8 directory data;
- truncated storage geometry;
- storage read failures.

The same portable test creates a 1 TiB logical sparse file in a 16 MiB memory image. It verifies allocation-free growth, out-of-order high/low/middle checksum-segment insertion, zero-filled reads, partial and full hole punching, exact reclamation, scrub behaviour, read-only reopen and rejection of unknown extent flags, physically backed holes and incomplete logical coverage.

`infilfs-hardening-conformance` covers checkpoint-label termination, canonical padding/reserved fields, object versions, read-only-compatible feature semantics, exact block ownership, namespace uniqueness/reachability/parentage, checksum-object reachability, read-status propagation, POSIX rename replacement, post-commit checkpoint-replica failure and explicit replica healing.

`infilfs-open-hardening` adds the 0.6.2 regression gates for a valid-CRC checkpoint whose bitmap is too small to represent `total_blocks` and for an attempted writable open on a backend that lacks mutation-critical callbacks. The malformed bitmap must be rejected before bit traversal.

`infilfs-063-hardening` adds deterministic 0.6.3 regression gates for full-graph fallback from corrupt newer checkpoints to an older committed generation, writable replica healing after fallback, refusal to mask a newer-generation I/O failure, and rename trailing-slash semantics.

`infilfs-sparse-files` adds deterministic transaction failpoints around high-offset writes and hole punches. It requires old-or-new visibility at the checkpoint boundary and checks repeated allocation/reclamation for block leaks.

GitHub Actions builds and tests the full Linux implementation, including the FUSE adapter, and separately builds the portable core with Microsoft Visual C on Windows. The hardening matrix also runs Clang, AddressSanitizer/UndefinedBehaviorSanitizer and GCC `-fanalyzer`; the Linux-side hardening jobs install the FUSE development headers so the adapter is compiled under those gates as well.

The high-level FUSE adapter deliberately uses libfuse's documented offsetless `readdir` mode: it ignores the incoming directory offset and passes zero offsets to the filler so libfuse consumes the complete directory in one operation. Synthetic resumable cookies are not part of the current adapter contract.

## Compatibility rule

A future change that alters golden checkpoint bytes, packed offsets, extent semantics, namespace representation, or the interpretation required by existing compatible/incompatible feature bits requires an explicit format revision or a newly defined compatibility feature bit as appropriate. Tightening rejection of metadata that Format 0.6 already defines as reserved, unreachable, multiply owned, structurally inconsistent or geometrically impossible is implementation hardening and does not create a new disk format.
