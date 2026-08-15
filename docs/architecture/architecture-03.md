<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Architecture — Part 3

Future versions will allow selected generations to remain addressable. Unlinked objects are not necessarily reclaimed immediately when retained generations still reference them.

This enables native snapshot, rollback and undelete semantics without raw-sector guessing.

## 10. Reflinks and shared extents

Two objects may reference the same immutable data extent. A reflink copy therefore initially consumes metadata only. When one copy is modified, the changed range receives new storage while unchanged extents remain shared.

Reference accounting must be transactional and recoverable. It will not be added until the simpler exclusive-ownership extent model is proven.

## 11. Workload-aware allocation

The allocator should eventually distinguish behavioural classes such as:

- static/archive;
- sequentially growing;
- streaming;
- random-write/database;
- temporary/cache;
- VM/disk image.

Classification can be inferred from access patterns, supplied by applications as a hint, or overridden administratively. It may affect extent size, locality, CoW policy, compression, write aggregation and protection level.

The stored data remains ordinary file data. Behaviour classes are policy hints, not application-specific filesystem formats.

## 12. Media awareness

The on-disk format should remain portable while allocation strategy adapts to storage characteristics.

Potential device classes include:

- rotational media, favouring physical locality and long sequential extents;
- SSD/NVMe, favouring parallelism, lower metadata contention and reduced write amplification;
- removable flash, favouring large aggregated writes and conservative metadata traffic;
- zoned storage, favouring zone-compatible sequential allocation.

## 13. Protection classes

A future file/object protection policy may express how much redundancy an object deserves rather than imposing one policy on the whole filesystem.

Examples:

- class 0: checksum only;
- class 1: ordinary protection;
- class 2: duplicated data when space and topology permit;
- class 3: strongest available redundancy.

On a single physical device, duplication cannot protect against complete device loss. The filesystem must never imply otherwise.

## 14. Compression and encryption

Compression is intended to operate per extent. Poorly compressible ranges may remain raw while useful ranges are compressed. The extent record identifies the encoding used.

Encryption is intended to operate at an object or encryption-domain level rather than requiring an opaque encrypted block device beneath the filesystem. Key wrapping should be separated from data encryption so user credential changes do not require rewriting all file data.

Neither feature belongs in the first writable prototype.

## 15. Verification

Format 0.4 provides `infilfs-scrub`, which opens a volume read-only and verifies all currently allocated regular-file data blocks. The scrub path reports checksum mismatches separately from checksum-metadata/structural errors. It is not yet coordinated with a concurrently writable mount; safe online scrub requires a stable snapshot/generation view or explicit mount coordination.

Repair is intentionally not attempted until the filesystem has a trustworthy redundant-copy model. Detecting corruption and inventing replacement bytes are separate operations.

## 16. Native filesystem transactions

The internal metadata transaction model may eventually be exposed to userspace so groups of namespace updates can become visible atomically. This is intentionally deferred until the core crash-consistency model is mature.
