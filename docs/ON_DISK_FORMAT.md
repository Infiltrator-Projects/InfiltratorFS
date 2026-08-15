<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS On-Disk Format 0.5

Status: experimental writable prototype. Format 0.5 is intentionally incompatible with formats 0.1 through 0.4.

Implementation release 0.5.1 adds byte-exact conformance tests and cross-platform compilation checks without changing this on-disk version. The normative conformance rules are summarized in `CONFORMANCE.md`.

## 1. Encoding

All integer fields are little-endian. The filesystem block size is 4096 bytes and the checkpoint records a block shift of 12. Metadata objects occupy one block in this prototype. File data is described by extents of 4096-byte blocks.

Structures in `include/infilfs/format.h` describe the exact packed record order and are guarded by compile-time size assertions. The byte format, not a compiler's native ABI, is authoritative.

## 2. Volume layout

For a volume of `N` blocks:

```text
block 0                 checkpoint A
block 1 .. B            allocation bitmap
block B+1               object-index object
block B+2               root-directory object
...                      metadata and file data
block floor(N/2)        checkpoint B
...                      metadata and file data
block N-1               checkpoint C
```

The bitmap contains one bit per filesystem block. Bits beyond the volume end are permanently marked unavailable.

## 3. Checkpoint

`struct infs_superblock_disk` contains:

```text
magic                       8 bytes: "INFS2026"
format major/minor          0.5
header size                 structure-size guard
block shift                 12
checksum algorithm          CRC64-ECMA
generation                  publication counter
total/free blocks           volume accounting
bitmap start/count          authoritative allocation bitmap
object-index block          current object-index root
root-object block           current namespace root
checkpoint blocks[3]        expected physical checkpoint positions
filesystem UUID             128-bit volume identity
root object ID              128-bit persistent root identity
feature flag sets           compat / read-only compatible / incompatible
label                       UTF-8, up to 63 bytes plus terminator
checksum field              32 bytes reserved
```

Format 0.5 requires `INFS_INCOMPAT_UTF8_NAMES`. Readers reject unknown incompatible feature flags.

CRC64 occupies the first eight checksum bytes. The complete checksum field is zero during calculation. A checkpoint is accepted only when its magic, format, size, block geometry, checksum, volume size, feature flags and expected checkpoint positions validate. The newest valid generation wins.

## 4. Allocation bitmap

Bit `b` describes block `b`: zero means free; one means allocated or unavailable. The bitmap is authoritative. Open recounts free blocks and rejects disagreement with the committed checkpoint.

Checkpoint, bitmap, object-index and root blocks must be allocated. Future free-extent indexes remain rebuildable accelerators.

## 5. Object header

Every metadata object begins with `struct infs_object_header_disk`:

```text
magic                       8 bytes: "INFOBJ01"
object type                 directory, file, index or checksum
object version              type-specific version
header size                 common-header size
generation                  most recent update generation
object ID                   128-bit persistent identity
parent ID                   namespace/recovery relationship
payload size                type-specific payload bytes
checksum algorithm          CRC64-ECMA
checksum                    32-byte reserved field
```

The entire metadata block is checksummed with the checksum field zeroed.

## 6. Object index

The index maps a persistent object ID to a physical metadata block and object type. Directory entries contain IDs rather than physical addresses, so relocation changes the index without rewriting every namespace reference.

Format 0.5 stores the index in one block. Entries are validated for duplicate IDs, bounds, allocation state, object identity, type and checksum. The root must appear exactly once and match the checkpoint.

## 7. Common attributes

`struct infs_attributes_disk` contains:

```text
logical size                    64-bit bytes
link count                      64-bit count
portable flags                  read-only, hidden, system, archive, etc.
birth time                      signed nanoseconds since Unix epoch
access time                     signed nanoseconds since Unix epoch
modification time               signed nanoseconds since Unix epoch
metadata-change time            signed nanoseconds since Unix epoch
security object ID              future security metadata, zero when absent
extended-attributes object ID   future named metadata, zero when absent
```

This record is independent of Linux `struct stat` and Windows file-information structures.

`struct infs_posix_compat_disk` follows the common attributes in current file and directory payloads. It contains permission bits, numeric UID/GID values and reserved flags. It is adapter metadata, not persistent object identity or the final cross-platform security model.

## 8. Directories and names

A directory payload contains common attributes, POSIX compatibility data, an entry count and the byte count occupied by variable-length records.

Each record contains its aligned record size, name length, target object type, flags, 128-bit target ID and name bytes. Names must be well-formed UTF-8, contain 1–255 bytes, and contain neither NUL nor `/`. Records are padded to eight-byte alignment.

Lookup in format 0.5 is case-sensitive and byte-exact. `.` and `..` are synthesized navigation components and are not stored.

## 9. Files, extents and checksums

A regular-file payload contains common attributes, POSIX compatibility data, extent count, data-checksum algorithm, checksum-chain head and ordered extent records.

Each extent records a logical start block, physical start block, block count and flags. Format 0.5 implements normal extents only. Logical blocks are contiguous from zero; writes beyond EOF allocate and zero the intervening range.

Each allocated logical block has one 32-byte checksum slot in a hidden checksum object. Format 0.5 fills the complete slot with SHA-256. Checksum objects identify their owner, covered logical range and next checksum object.

Checksums cover the complete physical 4096-byte data block, including zero-filled bytes beyond logical EOF. Reads verify before returning data. Shrink zeroes the retained final-block tail so later growth cannot expose truncated bytes.

## 10. Transaction publication

The storage backend must provide positioned read/write and a durable flush operation. Commit ordering is:

```text
write new unreachable metadata and data
durable flush
write new bitmap image
durable flush
write one generation N+1 checkpoint
durable flush                 <- atomic publication point
write remaining checkpoint copies
durable flush
```

The first checkpoint location rotates by generation. A crash before publication selects generation `N`; a crash after publication selects `N+1`. A writable open heals older checkpoint copies before further allocation.

Committed file-data blocks are also replaced through CoW. Extent mappings and independent checksum metadata publish in the same generation as the replacement data.

## 11. Corruption rejection

The opener rejects invalid checkpoint checksums or geometry, unsupported feature flags, inconsistent allocation accounting, reserved blocks marked free, malformed object payloads, invalid UTF-8 names, duplicate index identities, invalid object/index relationships, invalid extents, malformed checksum chains and data checksum mismatches.

The policy is to fail closed when committed state cannot be trusted.

## 12. Prototype limits

- one-block object index;
- one-block directories;
- no hard links or symbolic links;
- no sparse extents, compression, reflinks or snapshots;
- security and extended-attribute object references are reserved but not implemented;
- POSIX compatibility metadata exists; Windows security mapping is not implemented;
- metadata uses CRC64 while file data uses SHA-256;
- scrub detects but cannot yet repair corruption;
- one writable core instance at a time.
