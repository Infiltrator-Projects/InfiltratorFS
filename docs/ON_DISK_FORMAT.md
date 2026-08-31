<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS On-Disk Format 0.17

Status: experimental writable prototype. Release 0.18.28 accepts exactly Format 0.17. Release 0.18.19 is the last Format 0.16 release. Pre-1.0 development builds do not promise compatibility with earlier development formats.

Implementation 0.6.0 introduced sparse extents, sparse checksum indexing and hole punching. Implementation 0.7.0 defined `INFS_INCOMPAT_INLINE_DATA`. Implementation 0.8.0 added `INFS_INCOMPAT_SHARED_EXTENTS`; Format 0.8 added `INFS_INCOMPAT_PAGED_METADATA` and version-2 metadata heads. Format 0.9 added `INFS_INCOMPAT_SYMBOLIC_LINKS` and object type 5. Format 0.10 added `INFS_INCOMPAT_HARD_LINKS`. Format 0.12 added `INFS_INCOMPAT_SNAPSHOTS`, snapshot-catalog object type 6 and fixed-size generation-root records. Format 0.13 added `INFS_INCOMPAT_INDEX_TREE`, version-3 object-index heads and radix branch pages. Format 0.14 added `INFS_INCOMPAT_DIRECTORY_TREE`, version-3 directory heads and hashed directory branch pages. Format 0.15 doubled the maximum UTF-8 component length from 255 to 510 bytes without changing the variable-length directory-record layout. Format 0.16 raised that limit from 510 to 1023 bytes, again without changing the variable-length directory-record layout. Format 0.17 replaces the monolithic persistent allocation bitmap image with a checkpoint-rooted copy-on-write allocation tree while preserving one authoritative allocation bit per filesystem block. The normative acceptance rules are summarized in `CONFORMANCE.md`.

## 1. Encoding

All integer fields are little-endian. The filesystem block size is 4096 bytes and the checkpoint records a block shift of 12. Persistent objects occupy one block; current directory/index heads own checksummed tree pages and fragmented file heads may own checksummed extent pages. Extent-backed file data is described by extents of 4096-byte blocks; inline file data occupies otherwise unused bytes in the file metadata object.

Structures in `include/infilfs/format.h` describe the exact packed record order and are guarded by compile-time size assertions. The byte format, not a compiler's native ABI, is authoritative. Current reserved fields, metadata block tails and record padding are canonical zero. A reader must reject a CRC-valid object that uses current reserved space without a feature/version definition permitting it.

## 2. Volume layout

For a volume of `N` blocks, three fixed physical checkpoint locations remain at block 0, block `floor(N/2)` and block `N-1`. The checkpoint references the current allocation-map root, object-index head and namespace root. Allocation pages, directory/index pages, objects, extent metadata and file data otherwise occupy ordinary allocated blocks and may move between generations.

A newly formatted volume initially places the allocation root and its branch/leaf pages near the start of the volume followed by the initial object index and root directory, but those physical positions are formatter policy rather than permanent format addresses. Subsequent transactions copy on write only the allocation leaves whose bit payload changes and only affected branch paths plus the root needed to publish a new root.

The allocation map still contains exactly one authoritative bit per filesystem block. Bits beyond the volume end in the final in-memory byte are treated as unavailable. Inline bytes are part of an already allocated metadata object and therefore require no additional allocation ownership.

## 3. Checkpoint

`struct infs_superblock_disk` contains:

```text
magic                       8 bytes: "INFS2026"
format major/minor          0.17
header size                 structure-size guard
block shift                 12
checksum algorithm          CRC64-ECMA
generation                  publication counter
total/free blocks           volume accounting
allocation root/leaf count  authoritative allocation-tree bootstrap
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

Format 0.17 requires `INFS_INCOMPAT_UTF8_NAMES`, `INFS_INCOMPAT_SPARSE_EXTENTS` and `INFS_INCOMPAT_ALLOCATION_TREE`. Newly formatted volumes enable the complete known set: `INFS_INCOMPAT_INLINE_DATA`, `INFS_INCOMPAT_SHARED_EXTENTS`, `INFS_INCOMPAT_PAGED_METADATA`, `INFS_INCOMPAT_SYMBOLIC_LINKS`, `INFS_INCOMPAT_HARD_LINKS`, `INFS_INCOMPAT_SNAPSHOTS`, `INFS_INCOMPAT_PAGED_EXTENTS`, `INFS_INCOMPAT_INDEX_TREE`, `INFS_INCOMPAT_DIRECTORY_TREE` and `INFS_INCOMPAT_ALLOCATION_TREE`. The tree feature bits require matching version-3 object-index and directory heads. Without a tree bit, the corresponding classic or paged representation remains valid when its own feature contract agrees. Disagreement between a feature bit and the selected head version is corruption. Symbolic-link, hard-link and snapshot-catalog representations are accepted only with their corresponding feature bits. Readers reject a missing required bit or any unknown incompatible feature flag.

Format selection is exact during development: only Format 0.17 is accepted. Within that format, shared extents, inline data, paged extents, symbolic links, hard links and snapshot catalogs are interpreted only when their incompatible feature bits are present.

Feature classes have distinct compatibility semantics. Unknown incompatible bits prevent any open. Unknown read-only-compatible bits may be opened read-only but prevent writable open. Unknown compatible bits may be ignored. Format 0.17 defines no compatible or read-only-compatible bits for newly created volumes.

CRC64 occupies the first eight checksum bytes. The remaining checksum bytes are zero. The complete checksum field is zero during calculation. A physical checkpoint copy first passes its own magic, format, size, block geometry, checksum, volume size, feature flags, canonical padding and expected checkpoint-position checks. The implementation then validates the complete graph referenced by surviving checkpoint candidates in descending generation order. The newest candidate with a structurally valid committed graph wins; a corrupt newer graph may fall back to an older valid committed graph. External I/O, memory or unsupported-feature failures are not treated as graph corruption and therefore do not silently trigger fallback.

## 4. Allocation map

Bit `b` describes block `b`: zero means free; one means allocated or unavailable. The bits remain authoritative; Format 0.17 changes only how the live bitset is persisted.

The checkpoint stores `allocation_root_block` and `allocation_leaf_count`. The root is a level-3 `INFSAB01` branch page. Level-2 and level-1 branch pages also use `INFSAB01`; level-0 leaves use `INFSAL01`. Every allocation page contains generation, logical index, level, entry count, payload byte count, CRC64-ECMA type and a 32-byte checksum field whose first eight bytes carry CRC64 and whose remainder is zero.

The packed allocation-page header is 72 bytes. A 4096-byte page therefore has 4024 payload bytes. Each leaf owns exactly 32,192 allocation bits except the final leaf, which may contain fewer valid bits. Branch payloads contain little-endian 64-bit physical child pointers with fanout 503. Implementations that materialize branch-page locations in memory must preserve level grouping (root, all level-2 pages, then all level-1 pages); this is an implementation invariant used by copy-on-write publication and is qualified beyond the single-level-2 case. Format 0.17 fixes the root level at 3, giving a maximum addressable allocation geometry of 503^3 leaves. The declared leaf count must equal the count implied by `total_blocks`; missing, duplicate, out-of-range, checkpoint-overlapping or malformed allocation pages are corruption.

Allocation pages are bootstrap metadata rather than persistent objects and have no object IDs. Their generation may be older than the selected checkpoint because unchanged leaves are shared between committed generations. A page is valid only when its generation is nonzero and no newer than the checkpoint that references it, its logical index/level/entry count match its tree position, its reserved bytes are canonical zero and its CRC64 validates.

A transaction maintains the authoritative bitset in memory plus the rebuildable free-extent accelerator. It records allocated/deferred ranges rather than cloning the complete bitmap for rollback. At publication it computes the affected leaf set, closes that set over allocation-map pages whose own allocation state changes, reserves replacement pages before releasing old pages, writes only changed leaves, writes the affected replacement internal allocation-tree paths plus the root and finally publishes the new allocation root in the checkpoint. This removes volume-size-proportional bitmap rewrite and transaction-clone costs while preserving the existing CoW crash boundary.

The implementation reconstructs ownership from the committed roots. Every allocated in-volume block must be owned by a checkpoint, a current allocation-map page, current object-index head/page, directory page, indexed metadata object, normal file extent or retained snapshot graph. Metadata ownership overlap within one generation and allocated-but-unreachable blocks are corruption. Reuse across historical generations is expected. Normal data blocks may have multiple file owners within one generation only when `INFS_INCOMPAT_SHARED_EXTENTS` is set. Inline bytes remain owned by their file metadata object.

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

Classic metadata objects use object version 1. Format 0.8 directory and object-index heads used object version 2. Current Format 0.17 directory and object-index heads use object version 3 and reference tree pages. Versioned `infs_metadata_page_disk` blocks carry their own magic, owner identity, generation, entry count and CRC64. All object IDs and generations are nonzero. Bytes after declared payloads and unused checksum-field bytes are canonical zero.

## 6. Object index

The index maps a persistent object ID to a physical metadata block and object type. Directory entries contain IDs rather than physical addresses, so relocation changes the index without rewriting every namespace reference.

Format 0.17 stores the total entry count and one physical tree-root pointer in the version-3 index head. Each object-ID byte selects one slot in a 256-way branch page. Leaves reuse fixed-size ID-to-block index entries. Branch and leaf pages are independently allocation/owner/generation/checksum validated; branch pages store exactly 256 physical child pointers and their entry count equals the number of nonzero pointers. An index entry must follow the branch prefix selected by its object ID. Entries are validated for nonzero and duplicate IDs, bounds, allocation state, zero reserved flags, object identity, type and checksum. The root must appear exactly once and match the checkpoint.

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

Only the portable attribute flags currently defined in `format.h` are accepted. The generic security and extended-attribute references remain zero until their portable object classes and compatibility features are specified. Release 0.18.28 can persist standard Linux xattr namespaces and special-node details through Linux adapter metadata; those adapter sidecars do not consume these reserved portable references and do not make Linux metadata the cross-platform canonical model.

The current portable flags are persistent cross-platform metadata; mutation-policy semantics for flags such as `READ_ONLY` require an explicit core API/policy rather than being inferred silently by one adapter.

`struct infs_posix_compat_disk` follows the common attributes in current file and directory payloads. It contains permission bits, numeric UID/GID values and reserved flags. Permission bits are limited to `07777`; the current reserved flags field is zero. This is adapter compatibility metadata, not persistent object identity or the final cross-platform security model.

## 8. Directories and names

A Format 0.17 directory head contains common attributes, POSIX compatibility data, a total entry count, zero `bytes_used` and one physical tree-root pointer. The root pointer is zero exactly when the directory is empty. SHA-256 of the exact name bytes routes lookup through one byte per 256-way branch depth, with a maximum depth of 32. Leaves contain the ordinary variable-length directory records. Hashes select storage paths only; the exact UTF-8 name remains namespace identity and is compared byte-for-byte. Every branch and leaf page is allocation/owner/generation/checksum validated.

Each record contains its aligned record size, name length, target object type, flags, 128-bit target ID and name bytes. Names must be well-formed UTF-8, contain 1–1023 bytes, and contain neither NUL nor `/`. Records are padded to eight-byte alignment. Current record flags and padding are zero.

Lookup in Format 0.17 is case-sensitive and byte-exact. `.` and `..` are synthesized navigation components and are never stored. Pathnames ending in `/` retain directory semantics in namespace operations; rename does not silently strip a trailing slash from a non-directory source or nonexistent destination.

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

Each record stores a generation strictly older than the catalog's containing generation, creation time, immutable snapshot-bitmap start/count and free-block count, object-index block, root-object block and root object ID. Snapshot records deliberately retain a compact immutable flat ownership image even though the live generation uses the Format 0.17 allocation tree. That image owns its own blocks and describes the captured generation graph, but it excludes superseded live allocation-tree pages so snapshots do not pin obsolete allocator metadata. A captured graph may contain an older snapshot catalog; generation ordering therefore forms a strictly descending acyclic history.

Snapshot creation first publishes pending mutations, copies the stable generation ownership bitset, removes the live allocation-tree pages from that historical ownership image, adds the new immutable snapshot-bitmap blocks themselves, writes that flat image and adds the generation-root record in one CoW transaction. Normal later transactions consult the active catalog before releasing deferred blocks. A block remains allocated while any retained snapshot bitmap contains it.

Snapshot deletion removes the selected record and computes the union of the active graph and remaining snapshot bitmaps. Blocks unique to the deleted history are deferred until the new catalog, index, bitmap and checkpoint publish atomically. A newer snapshot may retain the graph of an older snapshot that existed when it was captured; deleting the visible older name therefore does not reclaim those dependencies until the newer snapshot is also deleted.

Snapshot views are read-only. Lookup, directory listing, attributes, symbolic-link targets and file reads use the captured root/index/bitmap rather than the active roots. Full graph validation and scrub recursively validate catalogued generation roots and verify retained file data. Rollback and undelete are not defined by Format 0.17.

## 11. Transaction publication and recovery

A writable storage backend must provide positioned write, durable flush, secure-random and current-time services in addition to read and size services. A mutation does not silently substitute a zero timestamp when the clock service fails.

Commit ordering is:

```text
write new unreachable metadata and data
durable flush
write changed allocation leaves and replacement internal allocation-tree paths plus the root
durable flush
write one generation N+1 checkpoint
durable flush                 <- atomic publication point
write remaining checkpoint copies
durable flush
```

The first checkpoint location rotates by generation. A crash before publication leaves generation `N` committed; a crash after publication leaves generation `N+1` committed. During open, each individually valid checkpoint candidate is validated through its referenced allocation tree, metadata, namespace and checksum graph before selection. If a newer candidate's committed graph is structurally corrupt, recovery may select the next older valid committed graph. A writable fallback then rewrites and durably flushes all three checkpoint copies to the selected generation before exposing the volume writable. A writable open does not perform fallback or healing when a physical checkpoint is unreadable, because that replica may be the only record of a newer committed generation. Read-only recovery may use surviving replicas and never modifies the media.

Once the first generation-N+1 checkpoint has been durably flushed, the mutation is committed even if later replication of secondary checkpoint copies fails. The implementation records degraded checkpoint redundancy and reports the namespace/data mutation as successful; a later explicit sync heals the replicas or reports the remaining storage error. A failure returned by the first checkpoint write or its durability flush leaves the outcome indeterminate, so the current opener permits no further mutation until close-and-reopen recovery selects the on-media generation.

Committed extent-backed file-data blocks are replaced through CoW. Inline file updates are committed through replacement of the file metadata object under the same CoW generation rules. Extent mappings, checksum metadata and inline/extent representation transitions publish atomically with the corresponding replacement data. Ordinary rename, including replacement of an existing file or empty directory, is one transaction and publishes no intermediate namespace.

## 12. Formatting safety and publication

`mkfs.infilfs` refuses a real block device without `--force`. It also refuses a mounted target, a whole-disk target with a mounted child partition, or a target/child held by another Linux block layer. If active-use status cannot be established safely, formatting a real block device fails closed.

The formatter reopens the exact block target with Linux `O_EXCL` after advisory mount/holder preflight, verifies that the device identity and geometry are unchanged, and retains that exclusive descriptor through the destructive write sequence. Failure to obtain exclusivity aborts formatting before the first write. Failure of the initial realtime-clock query is also a formatter error rather than silently creating zero initial timestamps. The formatter additionally takes the same nonblocking exclusive advisory lock used by writable POSIX volume openers, coordinating both image files and block targets with the formatter.

Formatting first invalidates the three candidate checkpoint locations and flushes that invalidation. It then writes the initial allocation leaves/branches, tree index and tree root directory, durably flushes those referenced structures, and only then publishes the three valid generation-1 checkpoints. Therefore an interrupted format is unmountable rather than presenting a valid checkpoint that references incomplete initial metadata. Release 0.18.28 creates Format 0.17 checkpoints with the complete current incompatible-feature set enabled.

## 13. Corruption rejection

The opener rejects invalid checkpoint checksums or geometry, an allocation-tree shape that cannot describe the declared volume, malformed/duplicate/out-of-range allocation pages, unsupported feature usage, inconsistent allocation accounting, noncanonical reserved data, invalid identities/generations, malformed object payloads, invalid UTF-8 names, stored navigation entries, duplicate names or index identities, dangling or multiply referenced namespace objects, wrong parent/type relationships, unreachable namespace objects, incorrect link counts, invalid snapshot names or generation ordering, malformed historical bitmap geometry/accounting, corrupt retained roots, physical-block ownership overlap within one generation, allocated-but-unreachable blocks, logical extent gaps/overlaps, unknown extent flags, holes with physical storage, normal extents without physical storage, malformed/unsorted/shared/orphaned checksum chains, malformed inline payloads, inline data on a volume without the inline feature bit, mismatched inline SHA-256 digests and extent-backed data checksum mismatches.

A corrupt newest checkpoint graph is not automatically fatal when an older independently valid committed checkpoint graph survives. Structural missing/cyclic internal references and repeated physical metadata-tree blocks are treated as graph corruption for candidate selection; checksummed aliasing does not make a DAG valid when tree semantics require unique ownership. Storage I/O failures, memory exhaustion and unsupported format/feature semantics are preserved as their actual failures and are not hidden by fallback.

The policy is to fail closed when committed state cannot be trusted while retaining a known-good older committed generation when the distributed checkpoint set proves one is available.

## 14. Prototype limits

- bounded tree depth and one-block branch fanout;
- paged extent maps remain bounded by the page-pointer capacity of one file head;
- at most 27 named snapshots in the bounded catalog;
- no snapshot rollback or native undelete policy;
- no compression;
- portable security and generic named-metadata object references are reserved but not yet standardized;
- Linux 0.18.28 development adapter xattrs/special-node metadata are not the final portable named-metadata/security model;
- POSIX compatibility metadata exists; Windows security mapping is not implemented;
- metadata uses CRC64 while file data uses SHA-256;
- scrub detects but cannot yet repair corruption; and
- one writable core instance at a time.
