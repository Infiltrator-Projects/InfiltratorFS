<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Architecture — Part 2

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

Format 0.4 implements ordered normal extents and already includes an extent flags field. Later formats can assign flags for states such as:

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

Formats 0.1 through 0.4 use CRC64-ECMA for metadata corruption detection and reserve a 32-byte metadata checksum field so stronger algorithms can be introduced without changing the common object shape.

Format 0.4 adds end-to-end file-data verification. Every allocated logical 4 KiB file block has a checksum stored in a hidden checksum object rather than beside the data block it authenticates. Checksum objects are themselves checksummed CoW metadata and are reached through the persistent object index. Each data-checksum slot is 32 bytes wide and format 0.4 fills it with a SHA-256 digest. The file payload retains an explicit checksum algorithm ID so future algorithms remain versionable.

Normal reads verify the complete data block before returning requested bytes. `infilfs-scrub` traverses every regular-file data block and independently checks it against the stored checksum. File data uses SHA-256; metadata objects and checkpoints still use CRC64-ECMA in this revision.

## 8. Recovery model

Recovery is designed into the format rather than treated as a separate emergency mechanism.

Metadata records should remain recognizable when encountered during a raw-device scan. Future recovery tooling should be able to enumerate object records, directory records, extent records and checkpoint records even if higher-level indexes are destroyed.

Where useful, limited redundant relationships may be retained. For example, a directory can map a name to an object while the object can retain a parent/name hint. One side is authoritative during normal operation; the redundant hint exists to improve forensic reconstruction.

## 9. Historical generations and snapshots

A snapshot is conceptually a retained committed root generation. Copy-on-write metadata makes this natural rather than a separate backup format.
