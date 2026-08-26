<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS On-Disk Format 0.12

Status: experimental writable prototype. Release 0.18.4 accepts exactly Format 0.12. Pre-1.0 development builds do not promise compatibility with any earlier development format, including 0.11.

Implementation 0.6.0 introduced sparse extents, sparse checksum indexing and hole punching. Implementation 0.7.0 defined `INFS_INCOMPAT_INLINE_DATA`. Implementation 0.8.0 added `INFS_INCOMPAT_SHARED_EXTENTS`; Format 0.8 added `INFS_INCOMPAT_PAGED_METADATA` and version-2 metadata heads. Format 0.9 added `INFS_INCOMPAT_SYMBOLIC_LINKS` and object type 5. Format 0.10 added `INFS_INCOMPAT_HARD_LINKS`. Format 0.12 adds `INFS_INCOMPAT_SNAPSHOTS`, snapshot-catalog object type 6 and fixed-size generation-root records. The normative acceptance rules are summarized in `CONFORMANCE.md`.

## 1. Encoding

All integer fields are little-endian. The filesystem block size is 4096 bytes and the checkpoint records a block shift of 12. Persistent objects occupy one block; Format 0.8 directory and index heads may own additional checksummed metadata pages. Extent-backed file data is described by extents of 4096-byte blocks; inline file data occupies otherwise unused bytes in the file metadata object.

Structures in `include/infilfs/format.h` describe the exact packed record order and are guarded by compile-time size assertions. The byte format, not a compiler's native ABI, is authoritative. Current reserved fields, metadata block tails and record padding are canonical zero. A reader must reject a CRC-valid object that uses current reserved space without a feature/version definition permitting it.

## 2. Volume layout

For a volume of `N` blocks:

```text
block 0                 checkpoint A
block 1 .. B            allocation bitmap
block B+1               object-index head
block B+2               first object-index metadata page
block B+3               root-directory head
...                      directory/index pages, objects and file data
block floor(N/2)        checkpoint B
...                      metadata and extent-backed file data
block N-1               checkpoint C
```

The bitmap contains one bit per filesystem block. Bits beyond the volume end are permanently marked unavailable. Inline bytes are part of an already allocated metadata object and therefore require no additional bitmap ownership.

## 3. Checkpoint

`struct infs_superblock_disk` contains:

```text
magic                       8 bytes: "INFS2026"
format major/minor          0.12
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

Format 0.12 requires `INFS_INCOMPAT_UTF8_NAMES` and `INFS_INCOMPAT_SPARSE_EXTENTS`. New volumes also enable `INFS_INCOMPAT_INLINE_DATA`, `INFS_INCOMPAT_SHARED_EXTENTS`, `INFS_INCOMPAT_PAGED_METADATA`, `INFS_INCOMPAT_SYMBOLIC_LINKS`, `INFS_INCOMPAT_HARD_LINKS` and `INFS_INCOMPAT_SNAPSHOTS`. When the paged feature is set, it requires version-2 directory and object-index heads; disagreement between the feature bit and either required head version is corruption. Symbolic-link, hard-link and snapshot-catalog representations are accepted only with their corresponding feature bits. Readers reject a missing required bit or any unknown incompatible feature flag.

Format selection is exact during development: only Format 0.12 is accepted. Within that format, shared extents, inline data, paged metadata, symbolic links, hard links and snapshot catalogs are interpreted only when their incompatible feature bits are present.

Feature classes have distinct compatibility semantics. Unknown incompatible bits prevent any open. Unknown read-only-compatible bits may be opened read-only but prevent writable open. Unknown compatible bits may be ignored. Format 0.12 defines no compatible or read-only-compatible bits for newly created volumes.

CRC64 occupies the first eight checksum bytes. The remaining checksum bytes are zero. The complete checksum field is zero during calculation. A physical checkpoint copy first passes its own magic, format, size, block geometry, checksum, volume size, feature flags, canonical padding and expected checkpoint-position checks. The implementation then validates the complete graph referenced by surviving checkpoint candidates in descending generation order. The newest candidate with a structurally valid committed graph wins; a corrupt newer graph may fall back to an older valid committed graph. External I/O, memory or unsupported-feature failures are not treated as graph corruption and therefore do not silently trigger fallback.

## 4. Allocation bitmap

Bit `b` describes block `b`: zero means free; one means allocated or unavailable. The bitmap is authoritative. Before any bitmap bit is traversed, the reader proves that the declared bitmap contains enough bits to represent every block in `total_blocks`. Open then recounts free blocks and rejects disagreement with the committed checkpoint.

The implementation reconstructs ownership from the committed roots. Every allocated in-volume block must be owned by a checkpoint, current bitmap image, current object-index head/page, directory page, indexed metadata object, normal file extent or retained snapshot graph. The active bitmap is the exact union of the live graph and each catalogued snapshot bitmap. Metadata ownership overlap within one generation and allocated-but-unreachable blocks are corruption. Reuse across historical generations is expected. Normal data blocks may have multiple file owners within one generation only when `INFS_INCOMPAT_SHARED_EXTENTS` is set. Inline bytes remain owned by their file metadata object.

## 5. Object header

Every metadata object begins with `struct infs_object_header_disk`:

```text
magic                       8 bytes: "INFOBJ01"
object type                 directory, file, index, checksum, symlink or snapshot catalog
object version              type-specific version
generation                  most recent update generation
object ID                   128-bit persistent identity
parent ID                   namespace/recovery relationship
payload size                type-specific payload bytes
checksum algorithm          CRC64-ECMA
checksum                    32-byte reserved field
```

Classic metadata objects use object version 1. Format 0.8 directory and object-index heads use object version 2 and reference versioned `infs_metadata_page_disk` blocks carrying their own magic, owner identity, generation, entry count and CRC64. All object IDs and generations are nonzero. Bytes after declared payloads and unused checksum-field bytes are canonical zero.

## 6. Object index

The index maps a persistent object ID to a physical metadata block and object type. Directory entries contain IDs rather than physical addresses, so relocation changes the index without rewriting every namespace reference.

Format 0.8 stores an array of physical index-page pointers in the version-2 index head. Each page contains fixed-size ID-to-block entries and is independently owner/generation/checksum validated. Entries are validated for nonzero and duplicate IDs, bounds, allocation state, zero reserved flags, object identity, type and checksum. The root must appear exactly once and match the checkpoint. The current head is a bounded one-level page array, not a tree.

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
security object ID              future portable security metadata, zero when absent
extended-attributes object ID   future portable named metadata, zero when absent
```

This record is independent of Linux `struct stat` and Windows file-information structures.

Only the portable attribute flags currently defined in `format.h` are accepted. The generic security and extended-attribute references remain zero until their portable object classes and compatibility features are specified. Release 0.18.4 can persist Linux `user.*` xattrs and special-node details through Linux adapter metadata; those adapter sidecars do not consume these reserved portable references and do not make Linux metadata the cross-platform canonical model.

The current portable flags are persistent cross-platform metadata; mutation-policy semantics for flags such as `READ_ONLY` require an explicit core API/policy rather than being inferred silently by one adapter.

`struct infs_posix_compat_disk` follows the common attributes in current file and directory payloads. It contains permission bits, numeric UID/GID values and reserved flags. Permission bits are limited to `07777`; the current reserved flags field is zero. This is adapter compatibility metadata, not persistent object identity or the final cross-platform security model.

## 8. Directories and names

A Format 0.8 directory head contains common attributes, POSIX compatibility data, a total entry count and physical pointers to independently checksummed directory pages. Each page stores variable-length records.

Each record contains its aligned record size, name length, target object type, flags, 128-bit target ID and name bytes. Names must be well-formed UTF-8, contain 1–255 bytes, and contain neither NUL nor `/`. Records are padded to eight-byte alignment. Current record flags and padding are zero.

Lookup in Format 0.12 is case-sensitive and byte-exact. `.` and `..` are synthesized navigation components and are never stored. Pathnames ending in `/` retain directory semantics in namespace operations; rename does not silently strip a trailing slash from a non-directory source or nonexistent destination.

With `INFS_INCOMPAT_HARD_LINKS`, a regular file may be referenced by one or more directory entries while retaining one indexed object, one data/checksum representation and one persistent object ID. Its stored link count equals its exact incoming directory-reference count. Creating the second name clears the file object's parent ID to zero because no single containing directory is authoritative; that zero remains valid if later unlink reduces the count to one. A file that has never been multiply linked may retain its sole containing directory ID. Directory and symbolic-link objects remain single-parent and singly referenced. Directory link count remains 2 plus its direct child-directory count. Hard links to directories or symbolic links are unsupported. Every namespace object remains root-reachable, every dirent type and identity must match the index, and names within each directory remain unique.

### Symbolic links

A symbolic-link object uses classic object version 1 and begins with common attributes and POSIX compatibility data, followed by a 32-bit target length and a zero reserved field. Exactly that many target bytes follow in the object payload. The target is nonempty, well-formed UTF-8, contains no NUL and is limited to 3,888 bytes. It may be absolute or relative. `logical_size` equals the target length, `link_count` is 1, and the portable core stores but does not resolve the target. Path resolution is the responsibility of the operating-system adapter.

## 9. Files, extents, inline data and checksums

A regular-file payload begins with common attributes, POSIX compatibility data, extent count, data-checksum algorithm and checksum-chain head. Format 0.8 retains the three Format 0.7 file shapes.

### Empty file

An empty file has logical size zero, extent count zero, a zero checksum-chain head and a payload consisting only of the fixed `struct infs_file_payload_disk`.

### Inline file

A non-empty inline file is valid only when the volume has `INFS_INCOMPAT_INLINE_DATA` set. It has:

- logical size from 1 through `INFS_INLINE_DATA_MAX` (currently 3,840 bytes);
- extent count zero;
- a zero checksum-chain head;
- data checksum type SHA-256;
- one 32-byte `struct infs_data_checksum_disk` immediately after the fixed file payload;
- exactly `logical_size` bytes of file data immediately after that digest; and
- no additional payload bytes.

The inline SHA-256 digest is calculated over one logical 4096-byte block: the inline bytes followed by zeros through byte 4095. This deliberately uses the same full-logical-block checksum rule as an extent-backed first data block. The enclosing metadata object's CRC64 independently covers the entire inline payload, including the digest and data bytes.

Inline bytes allocate no separate data blocks and no checksum objects, so their data-block contribution to allocated size is zero. Unwritten growth inside the inline range is represented by zero bytes. Hole punching inside an inline file zeroes the selected bytes while preserving logical size. Reads return the bytes directly after validating both the object and inline digest.

A write or truncate that grows an inline file beyond 3,840 bytes promotes it transactionally. The existing inline content is copied into a zero-padded normal data block, a normal extent and checksum-chain entry are created, and subsequent growth follows ordinary extent/sparse rules in the same transaction. A truncate that shrinks an extent-backed file to 3,840 bytes or less on an inline-enabled volume reads the retained prefix, stores it inline, and reclaims the previous normal extents and checksum chain before publication. Applications see no representation change through the file API.

### Extent-backed file

A non-empty extent-backed file has one or more ordered 24-byte extent records after the fixed file payload. Each extent records a logical start block, physical start block, 32-bit block count and flags. Extents are ordered and must cover logical blocks contiguously from zero through `ceil(logical_size / 4096)`. A zero block count, logical gap, logical overlap or unknown flag is corruption. Extent counts are bounded by the one-block metadata capacity before multiplication or pointer derivation.

Format 0.8 retains the Format 0.6 extent types:

- `INFS_EXTENT_NORMAL` (`flags == 0`): `physical_block` is nonzero and maps `block_count` allocated data blocks;
- `INFS_EXTENT_HOLE` (`flags == 1`): `physical_block` is zero and the logical range reads as zeros without data-block allocation.

Truncate growth beyond the inline representation appends or extends hole extents. A write into a hole replaces only each touched logical block with a normal extent. A full-block punch replaces the selected range with hole extents and preserves logical size. A partial-block punch keeps a normal block, zeroes the selected bytes through copy-on-write and updates its checksum. Adjacent compatible extents may be coalesced.

Each allocated normal logical block has one 32-byte SHA-256 slot in a hidden checksum object. Hole blocks require no checksum and are never verified as stored data. Checksum objects identify their owner and an aligned logical segment. Their linked list is sorted by strictly increasing segment start, but segments containing only holes may be absent. This permits one high-offset write without allocating checksum metadata for all preceding holes. Inactive checksum slots or objects may remain while the same file still owns other allocated blocks; the complete chain is reclaimed when the file has no allocated extent data.

Every indexed checksum object must be reachable exactly once from its owning extent-backed file's checksum head. Checksum owner ID and metadata parent ID must both identify that file. Shared, cyclic, duplicate or orphaned checksum objects are corruption. An inline or empty file has a zero checksum head.

With `INFS_INCOMPAT_SHARED_EXTENTS`, two or more regular files may refer to the same normal physical data blocks. Each file retains its own checksum chain. Writes replace only the changed logical blocks through CoW; truncate, hole punching, rename replacement and unlink reclaim a shared block only after no other committed file extent refers to it.

Extent-backed checksums cover the complete physical 4096-byte data block, including zero-filled bytes beyond logical EOF. Reads verify normal data before returning it and synthesize zeros directly for holes. Shrink zeroes the retained normal final-block tail so later growth cannot expose truncated bytes.

## 10. Snapshot catalog and retained generations

`INFS_INCOMPAT_SNAPSHOTS` permits exactly one snapshot-catalog object in the active object index. Its reserved 16-byte object ID is the byte string `INFS-SNAP-CAT-01`. The catalog uses classic object version 1, has a zero parent ID and stores a count followed by at most 27 fixed-size `infs_snapshot_record_disk` records. Snapshot names contain 1–63 well-formed UTF-8 bytes, contain neither NUL nor `/`, have canonical zero padding and are unique within the catalog.

Each record stores a generation strictly older than the catalog's containing generation, creation time, immutable bitmap start/count and free-block count, object-index block, root-object block and root object ID. The dedicated bitmap has the same geometry as the active bitmap, owns its own bitmap blocks and describes the complete captured generation graph. A captured graph may contain an older snapshot catalog; generation ordering therefore forms a strictly descending acyclic history.

Snapshot creation first publishes pending mutations, copies the stable generation bitmap to new immutable blocks, substitutes those new bitmap blocks for the superseded active bitmap blocks in the historical image, and adds the generation-root record in one CoW transaction. Normal later transactions consult the active catalog before releasing deferred blocks. A block remains allocated while any retained snapshot bitmap contains it.

Snapshot deletion removes the selected record and computes the union of the active graph and remaining snapshot bitmaps. Blocks unique to the deleted history are deferred until the new catalog, index, bitmap and checkpoint publish atomically. A newer snapshot may retain the graph of an older snapshot that existed when it was captured; deleting the visible older name therefore does not reclaim those dependencies until the newer snapshot is also deleted.

Snapshot views are read-only. Lookup, directory listing, attributes, symbolic-link targets and file reads use the captured root/index/bitmap rather than the active roots. Full graph validation and scrub recursively validate catalogued generation roots and verify retained file data. Rollback and undelete are not defined by Format 0.12.

## 11. Transaction publication and recovery

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

Committed extent-backed file-data blocks are replaced through CoW. Inline file updates are committed through replacement of the file metadata object under the same CoW generation rules. Extent mappings, checksum metadata and inline/extent representation transitions publish atomically with the corresponding replacement data. Ordinary rename, including replacement of an existing file or empty directory, is one transaction and publishes no intermediate namespace.

## 12. Formatting safety and publication

`mkfs.infilfs` refuses a real block device without `--force`. It also refuses a mounted target, a whole-disk target with a mounted child partition, or a target/child held by another Linux block layer. If active-use status cannot be established safely, formatting a real block device fails closed.

The formatter reopens the exact block target with Linux `O_EXCL` after advisory mount/holder preflight, verifies that the device identity and geometry are unchanged, and retains that exclusive descriptor through the destructive write sequence. Failure to obtain exclusivity aborts formatting before the first write. Failure of the initial realtime-clock query is also a formatter error rather than silently creating zero initial timestamps. The formatter additionally takes the same nonblocking exclusive advisory lock used by writable POSIX volume openers, coordinating both image files and block targets with the formatter.

Formatting first invalidates the three candidate checkpoint locations and flushes that invalidation. It then writes the bitmap, initial paged index and root, durably flushes those referenced structures, and only then publishes the three valid generation-1 checkpoints. Therefore an interrupted format is unmountable rather than presenting a valid checkpoint that references incomplete initial metadata. Implementations 0.16.0 and later create Format 0.12 checkpoints with inline data, shared extents, paged metadata, symbolic links, hard links and snapshots enabled.

## 13. Corruption rejection

The opener rejects invalid checkpoint checksums or geometry, an allocation bitmap too small for the declared volume, unsupported feature usage, inconsistent allocation accounting, noncanonical reserved data, invalid identities/generations, malformed object payloads, invalid UTF-8 names, stored navigation entries, duplicate names or index identities, dangling or multiply referenced namespace objects, wrong parent/type relationships, unreachable namespace objects, incorrect link counts, invalid snapshot names or generation ordering, malformed historical bitmap geometry/accounting, corrupt retained roots, physical-block ownership overlap within one generation, allocated-but-unreachable blocks, logical extent gaps/overlaps, unknown extent flags, holes with physical storage, normal extents without physical storage, malformed/unsorted/shared/orphaned checksum chains, malformed inline payloads, inline data on a volume without the inline feature bit, mismatched inline SHA-256 digests and extent-backed data checksum mismatches.

A corrupt newest checkpoint graph is not automatically fatal when an older independently valid committed checkpoint graph survives. Structural missing/cyclic internal references are treated as graph corruption for candidate selection. Storage I/O failures, memory exhaustion and unsupported format/feature semantics are preserved as their actual failures and are not hidden by fallback.

The policy is to fail closed when committed state cannot be trusted while retaining a known-good older committed generation when the distributed checkpoint set proves one is available.

## 14. Prototype limits

- bounded one-level object-index and directory page arrays rather than trees;
- at most 161 extents in one file metadata object;
- at most 27 named snapshots in the bounded catalog;
- no snapshot rollback or native undelete policy;
- no compression;
- portable security and generic named-metadata object references are reserved but not yet standardized;
- Linux 0.18.4 adapter xattrs/special-node metadata are not the final portable named-metadata/security model;
- POSIX compatibility metadata exists; Windows security mapping is not implemented;
- metadata uses CRC64 while file data uses SHA-256;
- scrub detects but cannot yet repair corruption; and
- one writable core instance at a time.
