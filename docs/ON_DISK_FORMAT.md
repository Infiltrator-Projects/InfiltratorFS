<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS On-Disk Format 0.2

Status: experimental writable prototype. The format is not frozen and is intentionally incompatible with the earlier 0.1 prototype.

## 1. Encoding and allocation unit

All integer fields are little-endian. The format-0.2 filesystem block size is fixed at 4096 bytes and the superblock records a block shift of 12.

Metadata objects occupy one filesystem block in format 0.2. File data is allocated in 4096-byte blocks described by extents.

## 2. Volume layout

For a volume of `N` blocks:

```text
block 0                 checkpoint/superblock A
block 1 .. B            allocation bitmap
block B+1               object-index object
block B+2               root-directory object
...                      allocated metadata and file-data blocks
block floor(N/2)        checkpoint/superblock B
...                      allocated metadata and file-data blocks
block N-1               checkpoint/superblock C
```

The bitmap is large enough for one bit per filesystem block. Bits beyond the actual end of the volume in the final bitmap block are permanently marked allocated/unavailable.

## 3. Superblock/checkpoint

`struct infs_superblock_disk` records:

```text
magic                   8 bytes: "INFS2026"
format major/minor      current format is 0.2
header size             structure-size guard
block shift             12
checksum type           CRC64-ECMA
generation              filesystem publication counter
total/free blocks       volume/accounting information
bitmap start/count      authoritative allocation bitmap
object index block      physical location of the object-index object
root object block       physical location of the current root directory
checkpoint blocks[3]    expected checkpoint positions
filesystem UUID         128-bit volume identity
root object ID          128-bit persistent root identity
feature flag sets       compat / read-only compatible / incompatible
label                   up to 63 bytes plus terminator in current tools
checksum field          32 bytes reserved for checksum data
```

CRC64-ECMA occupies the first eight bytes of the checksum field. The complete checksum field is treated as zero during checksum calculation.

A checkpoint is accepted only if its magic, format, structure size, block size, checksum algorithm, checksum, volume size and expected checkpoint positions validate. The reader selects the valid checkpoint with the greatest generation number.

## 4. Allocation bitmap

Bit `b` describes filesystem block `b`:

```text
0 = free
1 = allocated or unavailable
```

The bitmap is authoritative. On open, format 0.2 recounts free blocks and rejects the volume if the bitmap disagrees with the committed `free_blocks` value. Bitmap storage blocks, checkpoints, the object index and root object must all be marked allocated.

Future free-extent indexes are accelerators only and must remain rebuildable from this bitmap.

## 5. Metadata object header

All metadata objects begin with `struct infs_object_header_disk`:

```text
magic                   8 bytes: "INFOBJ01"
object type             directory, regular file or object index
object version          type-specific version
header size             common-header size
generation              most recent object update generation
object ID               128-bit persistent identity
parent ID               recovery/namespace relationship for files/directories
payload size            bytes of type-specific payload
checksum type           CRC64-ECMA
checksum                 32-byte reserved checksum field
```

The entire 4096-byte metadata block is checksummed with its checksum field zeroed. Payload lengths and records are bounds checked before use.

## 6. Persistent object index

The object index maps persistent object IDs to the physical blocks currently containing their metadata objects.

The payload begins with:

```text
entry_count             32-bit count
reserved                32-bit
```

followed by fixed-size entries:

```text
object_id               128-bit identity
object_block            64-bit physical block
object_type             directory or file
flags                    reserved
reserved                reserved
```

Directories therefore do **not** encode physical object locations. Moving a metadata object can eventually be implemented by changing the object index without rewriting every namespace entry that names that object.

Format 0.2 stores the index in one block, which intentionally limits the prototype to roughly one hundred live namespace objects. A later tree-based index will remove this limit.

On open, every index entry is checked for duplicate IDs, valid allocated block placement, matching object ID, matching object type and valid object checksum. The root directory must appear exactly once and match the root identity and block stored in the superblock.

## 7. Common file/directory attributes

`struct infs_stat_disk` stores:

```text
mode                    POSIX type and permission bits
uid / gid               owner identities
nlink                   link count
size                    logical file size in bytes
atime_ns                 access timestamp storage
mtime_ns                 modification timestamp
ctime_ns                 metadata-change timestamp
flags                    reserved for future object policy flags
```

Times are nanoseconds since the Unix epoch.

## 8. Directories

A directory payload contains its common attributes, an entry count and the number of bytes occupied by variable-length directory records.

Each directory record contains:

```text
record_size             total record bytes, padded to 8-byte alignment
name_length             filename byte length
object_type             file or directory
flags                    reserved
object_id               128-bit target object identity
name                     raw name bytes, no terminating NUL
padding                  zero-filled to 8-byte alignment
```

Names are limited to 255 bytes in format 0.2. NUL and `/` are invalid in stored names. `.` and `..` are synthesized by the VFS/FUSE layer and are not stored as directory entries.

A format-0.2 directory occupies one metadata block. This is a prototype scalability limit, not a long-term design rule.

## 9. Regular files and extents

A regular-file payload contains common attributes, an extent count and an ordered array of extent records.

Each extent stores:

```text
logical_block           first logical file block
physical_block          first physical filesystem block
block_count             number of consecutive blocks
flags                    extent encoding/policy flags
```

Format 0.2 implements normal extents only. The logical extent sequence must be contiguous from logical block zero; sparse extents are deferred. Writes beyond EOF therefore allocate the intervening blocks and initialize unwritten bytes to zero.

The allocator searches the authoritative bitmap for contiguous free runs. Adjacent logical and physical allocations are coalesced into the previous extent when possible. Multiple extents are supported when fragmentation prevents one contiguous run.

Shrinking a file releases blocks beyond the new EOF and zeroes the unused tail of the retained final block so a later regrow cannot expose previously truncated data.

## 10. Namespace operations

Format 0.2 supports:

- lookup by pathname;
- file creation;
- directory creation;
- file reads and writes;
- truncate/grow/shrink;
- unlink;
- removal of empty directories;
- rename within a directory;
- rename across directories;
- mode, ownership and timestamp changes.

Cross-directory rename updates the child object's persistent parent ID. Directory link counts are updated when subdirectories are created, removed or moved.

## 11. Generation and durability status

Every successful mutating operation writes changed metadata, writes the current bitmap, increments the superblock generation, rewrites all three checkpoint copies and calls `fsync()`.

This provides durable persistence during ordinary operation, but **format 0.2 is not crash-transactional**. Metadata objects and the bitmap are still updated in place. A power loss between individual writes can therefore leave a generation inconsistent.

Phase 2 replaces this update model with copy-on-write metadata and atomic generation publication. Format 0.2 must not be described as power-loss safe.

## 12. Corruption rejection

The current opener rejects, among other conditions:

- invalid checkpoint checksums or geometry;
- inconsistent bitmap/free-block accounting;
- reserved metadata blocks marked free;
- invalid root object ID or parent relationship;
- invalid metadata checksums;
- malformed object payload lengths;
- malformed directory record lengths or names;
- duplicate object-index identities;
- index entries pointing outside the volume or to free blocks;
- object/index type or identity mismatches;
- extents with invalid logical ordering or physical bounds.

The intent is to fail closed rather than continue with metadata that cannot be trusted.

## 13. Deliberate format-0.2 limits

The following are intentionally deferred rather than hidden:

- one-block object index;
- one-block directories;
- no hard links;
- no symbolic links;
- no sparse extent representation;
- no data checksums yet;
- no compression, reflinks or snapshots;
- no crash-consistent CoW metadata yet;
- single-writer FUSE operation.

These constraints keep Phase 1 small enough to verify before the Phase 2 transaction model changes how metadata is published.
