<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Architecture

## 1. Purpose

InfiltratorFS is a clean-sheet Linux filesystem intended to explore what a general-purpose filesystem should look like when designed in 2026 without legacy on-disk compatibility constraints.

The design is guided by four assumptions:

1. storage can return incorrect data;
2. power loss and interrupted writes are ordinary failure modes;
3. filesystem metadata should be recoverable without trusting a single root structure;
4. placement, protection and write policy should reflect how data is actually used.

## 2. Fundamental model

The filesystem is object-oriented internally. Files, directories, symlinks, snapshots and future metadata classes are persistent objects identified by 128-bit object IDs.

A pathname is a namespace mapping, not the identity of an object. A directory maps names to object IDs. Renaming a file therefore changes namespace metadata without changing the file object's identity.

Objects are located through metadata trees in later format revisions. Format 0.1 stores the root object's physical block directly in the superblock to keep the initial implementation small.

## 3. Transaction model

Critical metadata is never modified in place as the only valid copy.

A future transaction follows this model:

1. start from committed generation N;
2. write replacement metadata blocks elsewhere;
3. write and verify checksums;
4. write updated metadata roots;
5. publish generation N+1 by updating checkpoint records;
6. only then permit old, unreferenced metadata to be reclaimed.

After power loss the mount code selects the newest internally consistent checkpoint generation. A partially written future generation is ignored.

## 4. Checkpoints

The filesystem keeps multiple superblock/checkpoint copies at physically separated positions. Format 0.1 uses three copies:

- block 0;
- midpoint block;
- final block.

Later revisions may increase the number of checkpoints or use rotating checkpoint slots. Each checkpoint contains a generation number, format version, UUID, root pointer, allocation-map location, feature flags and checksum.

The mount rule is not merely "take the first superblock that parses". The implementation validates candidate checkpoints and chooses the newest generation satisfying format and volume-consistency rules.

## 5. Free-space model

InfiltratorFS deliberately combines two complementary views of free space.

### Allocation bitmap

One bit represents one 4 KiB block. The bitmap provides direct exact knowledge of whether any individual block is allocated.

### Free-extent index

A later revision adds a tree of free ranges, allowing rapid discovery of large contiguous areas without scanning millions of bitmap bits.

The bitmap is authoritative for exact allocation state. The free-extent index is an accelerator that can be rebuilt from the bitmap if damaged.

This asymmetry is intentional: important state has a compact, reconstructable source of truth while high-performance indexes remain disposable.

## 6. File allocation

File data uses extents rather than linked block chains. An extent describes a logical file range mapped to a physical block range.

Future extent records reserve space for flags allowing states such as:

- normal;
- sparse;
- compressed;
- shared/reflinked;
- inline;
- explicitly non-CoW;
- integrity class;
- storage-placement class.

This permits later features without redesigning the basic file mapping model.

## 7. Integrity

Every metadata block is self-identifying and checksummed.

The common conceptual metadata header contains:

- magic/type;
- structure version;
- object ID where applicable;
- generation;
- payload length;
- checksum algorithm;
- checksum.

Format 0.1 uses CRC64-ECMA for corruption detection and reserves a 32-byte checksum field so stronger algorithms can be introduced without changing the overall structure shape.

The long-term design calls for end-to-end data checksums. A checksum should be stored independently enough from the protected data that corruption of the data does not silently corrupt the only record used to authenticate it.

## 8. Recovery model

Recovery is designed into the format rather than treated as a separate emergency mechanism.

Metadata records should remain recognizable when encountered during a raw-device scan. Future recovery tooling should be able to enumerate object records, directory records, extent records and checkpoint records even if higher-level indexes are destroyed.

Where useful, limited redundant relationships may be retained. For example, a directory can map a name to an object while the object can retain a parent/name hint. One side is authoritative during normal operation; the redundant hint exists to improve forensic reconstruction.

## 9. Historical generations and snapshots

A snapshot is conceptually a retained committed root generation. Copy-on-write metadata makes this natural rather than a separate backup format.

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

## 15. Online verification

The intended operational model includes mounted verification rather than reserving consistency checking for an offline `fsck` event.

Future tooling should support independent metadata and data scrubs, with repair only when a trustworthy redundant copy exists.

## 16. Native filesystem transactions

The internal metadata transaction model may eventually be exposed to userspace so groups of namespace updates can become visible atomically. This is intentionally deferred until the core crash-consistency model is mature.

## 17. Non-goals

The project intentionally rejects several design directions:

- no FAT-style linked allocation chains;
- no fixed-size global inode table as a fundamental identity mechanism;
- no single irreplaceable superblock;
- no unchecksummed critical metadata;
- no assumption that successful I/O implies correct data;
- no global always-on deduplication in the synchronous write path;
- no dependence on FUSE in the on-disk format;
- no attempt to preserve compatibility with another filesystem's disk structures.
