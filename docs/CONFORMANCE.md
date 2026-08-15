<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Format 0.6 Conformance

Implementation 0.6.0 defines the portable contract of on-disk Format 0.6. Format 0.6 is intentionally incompatible with Format 0.5 because hole extents and sparse checksum chains change required interpretation and validation.

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
- strict rejection of overlong UTF-8, surrogate code points, truncated sequences and values above U+10FFFF;
- byte-exact, case-sensitive namespace comparison for Format 0.6;
- 128-bit persistent volume and object identities;
- common attributes and isolated POSIX compatibility metadata.
- normal extents with nonzero physical locations and hole extents with physical location zero;
- contiguous logical extent coverage through the rounded-up logical file size;
- zero reads and zero allocated-size contribution for holes;
- SHA-256 entries only for allocated data blocks, addressed by sorted sparse checksum chains.

## Portable result contract

The core and storage-service boundary return `infs_status` values. Successful operations return `INFS_STATUS_OK`; failures return a stable negative value such as `INFS_STATUS_CORRUPT`, `INFS_STATUS_NOT_FOUND` or `INFS_STATUS_NO_SPACE`. Byte-count APIs return a non-negative count or a negative `infs_status`.

Operating-system adapters perform native translation. The current POSIX adapter maps between `infs_status` and `errno`. A future Windows adapter can map the same values to Win32 errors or NTSTATUS without changing the core or disk format.

## Automated checks

`infilfs-format-conformance` checks exact structure sizes and offsets, independently constructs the expected checkpoint bytes, and rejects incompatible versions, altered geometry, unsupported feature flags, checksum damage and malformed UTF-8.

`infilfs-volume-conformance` constructs a deterministic complete volume in memory. It verifies the valid image, checkpoint redundancy and rejection of:

- three damaged checkpoints;
- unknown incompatible features;
- incorrect free-space accounting;
- a critical block marked free;
- a mismatched root identity;
- malformed UTF-8 directory data;
- truncated storage geometry;
- storage read failures.

The same portable test creates a 1 TiB logical sparse file in a 16 MiB memory image. It verifies allocation-free growth, out-of-order high/low/middle checksum-segment insertion, zero-filled reads, partial and full hole punching, exact reclamation, scrub behaviour, read-only reopen and rejection of unknown flags, physically backed holes and incomplete logical coverage.

`infilfs-sparse-files` adds deterministic transaction failpoints around high-offset writes and hole punches. It requires old-or-new visibility at the checkpoint boundary and checks repeated allocation/reclamation for block leaks.

GitHub Actions builds and tests the full Linux implementation and separately builds the portable core with Microsoft Visual C on Windows. Windows CI does not claim a Windows filesystem driver; it proves that the format/core boundary is not tied to Linux compilation.

## Compatibility rule

Any future change that alters the golden checkpoint bytes, packed offsets, extent semantics, required namespace rules or accepted incompatible features must be treated as an explicit format revision. It must not be released silently under Format 0.6.
