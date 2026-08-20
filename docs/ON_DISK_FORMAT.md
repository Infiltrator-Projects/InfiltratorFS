<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS On-Disk Format 0.6

Status: experimental writable prototype. Format 0.6 is intentionally incompatible with formats 0.1 through 0.5.

Implementation 0.6.0 introduced sparse extents, sparse checksum indexing and hole punching. Implementation 0.6.1 hardened structural validation and namespace/integrity semantics. Implementation 0.6.2 closed post-release source-level geometry, error-propagation, adapter and destructive-tool safety defects. Implementation 0.6.3 added complete checkpoint-graph recovery selection. Implementation 0.6.4 adds fail-closed checkpoint publication and POSIX storage locking without changing any Format 0.6 packed field, offset or feature identity. The normative acceptance rules are summarized in `CONFORMANCE.md`.

## 1. Encoding

All integer fields are little-endian. The filesystem block size is 4096 bytes and the checkpoint records a block shift of 12. Metadata objects occupy one block in this prototype. File data is described by extents of 4096-byte blocks.

Structures in `include/infilfs/format.h` describe the exact packed record order and are guarded by compile-time size assertions. The byte format, not a compiler's native ABI, is authoritative. Current reserved fields, metadata block tails and record padding are canonical zero. A reader must reject a CRC-valid object that uses current reserved space without a feature/version definition permitting it.

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
format major/minor          0.6
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

Generation, filesystem UUID and root object ID are nonzero.

Format 0.6 requires both `INFS_INCOMPAT_UTF8_NAMES` and `INFS_INCOMPAT_SPARSE_EXTENTS`. Readers reject a missing required bit or any unknown incompatible feature flag. A sparse-extents bit on a pre-0.6 checkpoint is invalid.

Feature classes have distinct compatibility semantics. Unknown incompatible bits prevent any open. Unknown read-only-compatible bits may be opened read-only but prevent writable open. Unknown compatible bits may be ignored. Implementation 0.6.4 currently defines no compatible or read-only-compatible bits for newly created Format 0.6 volumes.

CRC64 occupies the first eight checksum bytes. The remaining checksum bytes are zero in Format 0.6. The complete checksum field is zero during calculation. A physical checkpoint copy first passes its own magic, format, size, block geometry, checksum, volume size, feature flags, canonical padding and expected checkpoint-position checks. Implementations 0.6.3 and later then validate the complete graph referenced by surviving checkpoint candidates in descending generation order. The newest candidate with a structurally valid committed graph wins; a corrupt newer graph may fall back to an older valid committed graph. External I/O, memory or unsupported-feature failures are not treated as graph corruption and therefore do not silently trigger fallback.

## 4. Allocation bitmap

Bit `b` describes block `b`: zero means free; one means allocated or unavailable. The bitmap is authoritative. Before any bitmap bit is traversed, the reader proves that the declared bitmap contains enough bits to represent every block in `total_blocks`. Open then recounts free blocks and rejects disagreement with the committed checkpoint.

Implementations 0.6.1 and later additionally reconstruct ownership from the committed roots. Every allocated in-volume block must be owned exactly once by a checkpoint, current bitmap image, current object-index root, indexed metadata object or normal file extent. An overlap or an allocated-but-unreachable block is corruption. Future shared/reflink extents require an explicit format feature and reference-accounting rules before duplicate data ownership can become valid.

## 5. Object header

Every metadata object begins with `struct infs_object_header_disk`:

```text
magic                       8 bytes: "INFOBJ01"
object type                 directory, file, index or checksum
object version              type-specific version
generation                  most recent update generation
object ID                   128-bit persistent identity
parent ID                   namespace/recovery relationship
payload size                type-specific payload bytes
checksum algorithm          CRC64-ECMA
checksum                    32-byte reserved field
```

Format 0.6 metadata objects use object version 1, nonzero object IDs and nonzero generations. The entire metadata block is checksummed with the checksum field zeroed. Bytes after the declared payload and unused checksum-field bytes are canonical zero.

## 6. Object index

The index maps a persistent object ID to a physical metadata block and object type. Directory entries contain IDs rather than physical addresses, so relocation changes the index without rewriting every namespace reference.

Format 0.6 stores the index in one block. Entries are validated for nonzero and duplicate IDs, bounds, allocation state, zero reserved flags, object identity, type and checksum. The index object's parent ID and reserved payload field are zero. The root must appear exactly once and match the checkpoint.

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

Implementations 0.6.1 and later accept only the portable attribute flags currently defined in `format.h`. Security and extended-attribute references remain zero until their object classes and compatibility feature are specified. The current portable flags are persistent cross-platform metadata; mutation-policy semantics for flags such as `READ_ONLY` require a future explicit core API/policy rather than being inferred silently by one adapter.

`struct infs_posix_compat_disk` follows the common attributes in current file and directory payloads. It contains permission bits, numeric UID/GID values and reserved flags. Permission bits are limited to `07777`; the current reserved flags field is zero. This is adapter metadata, not persistent object identity or the final cross-platform security model.

## 8. Directories and names

A directory payload contains common attributes, POSIX compatibility data, an entry count and the byte count occupied by variable-length records.

Each record contains its aligned record size, name length, target object type, flags, 128-bit target ID and name bytes. Names must be well-formed UTF-8, contain 1–255 bytes, and contain neither NUL nor `/`. Records are padded to eight-byte alignment. Current record flags and padding are zero.

Lookup in Format 0.6 is case-sensitive and byte-exact. `.` and `..` are synthesized navigation components and are never stored. Pathnames ending in `/` retain directory semantics in namespace operations; rename does not silently strip a trailing slash from a regular-file source or nonexistent destination.

Format 0.6 has no hard links. Therefore every non-root file or directory must be referenced by exactly one directory entry. Every directory entry must resolve to an indexed object of the declared type, and the child's stored parent ID must identify the containing directory. Names within a directory are unique. Every file/directory must be reachable from the root; the root itself has no parent and no incoming namespace entry. File link count is 1; directory link count is 2 plus its direct child-directory count. These rules also make disconnected directory cycles invalid committed state.

## 9. Files, extents and checksums

A regular-file payload contains common attributes, POSIX compatibility data, extent count, data-checksum algorithm, checksum-chain head and ordered extent records.

Each 24-byte extent records a logical start block, physical start block, 32-bit block count and flags. Extents are ordered and must cover logical blocks contiguously from zero through `ceil(logical_size / 4096)`. A zero block count, logical gap, logical overlap or unknown flag is corruption. Extent counts are bounded by the one-block metadata capacity before multiplication or pointer derivation.

Format 0.6 defines:

- `INFS_EXTENT_NORMAL` (`flags == 0`): `physical_block` is nonzero and maps `block_count` allocated data blocks;
- `INFS_EXTENT_HOLE` (`flags == 1`): `physical_block` is zero and the logical range reads as zeros without data-block allocation.

Truncate growth appends or extends hole extents. A write into a hole replaces only each touched logical block with a normal extent. A full-block punch replaces the selected range with hole extents and preserves logical size. A partial-block punch keeps a normal block, zeroes the selected bytes through copy-on-write and updates its checksum. Adjacent compatible extents may be coalesced.

Each allocated normal logical block has one 32-byte SHA-256 slot in a hidden checksum object. Hole blocks require no checksum and are never verified as stored data. Checksum objects identify their owner and an aligned logical segment. Their linked list is sorted by strictly increasing segment start, but segments containing only holes may be absent. This permits one high-offset write without allocating checksum metadata for all preceding holes. Inactive checksum slots or objects may remain while the same file still owns other allocated blocks; the complete chain is reclaimed when the file has no allocated data.

Every indexed checksum object must be reachable exactly once from its owning file's checksum head. Checksum owner ID and metadata parent ID must both identify that file. Shared, cyclic, duplicate or orphaned checksum objects are corruption. A file with no allocated data has a zero checksum head.

Checksums cover the complete physical 4096-byte data block, including zero-filled bytes beyond logical EOF. Reads verify normal data before returning it and synthesize zeros directly for holes. Shrink zeroes the retained normal final-block tail so later growth cannot expose truncated bytes.

## 10. Transaction publication and recovery

A writable storage backend must provide positioned write, durable flush, secure-random and current-time services in addition to read and size services. A mutation does not silently substitute a zero timestamp when the clock service fails.

Commit ordering is:

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

The first checkpoint location rotates by generation. A crash before publication leaves generation `N` committed; a crash after publication leaves generation `N+1` committed. During open, each individually valid checkpoint candidate is validated through its referenced bitmap, metadata, namespace and checksum graph before selection. If a newer candidate's committed graph is structurally corrupt, recovery may select the next older valid committed graph. A writable fallback then rewrites and durably flushes all three checkpoint copies to the selected generation before exposing the volume writable. A writable open does not perform fallback or healing when a physical checkpoint is unreadable, because that replica may be the only record of a newer committed generation. Read-only recovery may use surviving replicas and never modifies the media.

Once the first generation-N+1 checkpoint has been durably flushed, the mutation is committed even if later replication of secondary checkpoint copies fails. The implementation records degraded checkpoint redundancy and reports the namespace/data mutation as successful; a later explicit sync heals the replicas or reports the remaining storage error. A failure returned by the first checkpoint write or its durability flush leaves the outcome indeterminate, so the current opener permits no further mutation until close-and-reopen recovery selects the on-media generation.

Committed file-data blocks are replaced through CoW. Extent mappings and independent checksum metadata publish in the same generation as replacement data. Ordinary rename, including replacement of an existing file or empty directory, is one transaction and publishes no intermediate namespace.

## 11. Formatting safety and publication

`mkfs.infilfs` refuses a real block device without `--force`. It also refuses a mounted target, a whole-disk target with a mounted child partition, or a target/child held by another Linux block layer. If active-use status cannot be established safely, formatting a real block device fails closed.

Implementation 0.6.2 and later reopen the exact block target with Linux `O_EXCL` after advisory mount/holder preflight, verify that the device identity and geometry are unchanged, and retain that exclusive descriptor through the destructive write sequence. Failure to obtain exclusivity aborts formatting before the first write. Implementation 0.6.3 also treats failure of the initial realtime-clock query as a formatter error rather than silently creating zero initial timestamps. Implementation 0.6.4 additionally takes the same nonblocking exclusive advisory lock used by writable POSIX volume openers, coordinating both image files and block targets with the formatter.

Formatting first invalidates the three candidate checkpoint locations and flushes that invalidation. It then writes the bitmap, initial index and root, durably flushes those referenced structures, and only then publishes the three valid generation-1 checkpoints. Therefore an interrupted format should be unmountable rather than presenting a valid checkpoint that references incomplete initial metadata.

## 12. Corruption rejection

The opener rejects invalid checkpoint checksums or geometry, an allocation bitmap too small for the declared volume, unsupported feature usage, inconsistent allocation accounting, noncanonical reserved data, invalid identities/generations, malformed object payloads, invalid UTF-8 names, stored navigation entries, duplicate names or index identities, dangling or multiply referenced namespace objects, wrong parent/type relationships, unreachable namespace objects, incorrect link counts, physical-block ownership overlap, allocated-but-unreachable blocks, logical extent gaps/overlaps, unknown extent flags, holes with physical storage, normal extents without physical storage, malformed/unsorted/shared/orphaned checksum chains and data checksum mismatches.

A corrupt newest checkpoint graph is not automatically fatal when an older independently valid committed checkpoint graph survives. Structural missing/cyclic internal references are treated as graph corruption for candidate selection. Storage I/O failures, memory exhaustion and unsupported format/feature semantics are preserved as their actual failures and are not hidden by fallback.

The policy is to fail closed when committed state cannot be trusted while retaining a known-good older committed generation when the distributed checkpoint set proves one is available.

## 13. Prototype limits

- one-block object index;
- one-block directories;
- no hard links or symbolic links;
- no compression, reflinks or snapshots;
- security and extended-attribute object references are reserved but not implemented;
- POSIX compatibility metadata exists; Windows security mapping is not implemented;
- metadata uses CRC64 while file data uses SHA-256;
- scrub detects but cannot yet repair corruption;
- one writable core instance at a time.
