<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS On-Disk Format 0.4 — Part 2

The entire 4096-byte metadata block is checksummed with its checksum field zeroed. Payload lengths and records are bounds checked before use.

## 6. Persistent object index

The object index maps persistent object IDs to the physical blocks currently containing their metadata objects. Linux-visible inode numbers are derived from the persistent object ID rather than this mutable physical mapping.

The payload begins with:

```text
entry_count             32-bit count
reserved                32-bit
```

followed by fixed-size entries:

```text
object_id               128-bit identity
object_block            64-bit physical block
object_type             directory, file or hidden checksum object
flags                    reserved
reserved                reserved
```

Directories therefore do **not** encode physical object locations. Format 0.4 moves metadata objects by changing the object index without rewriting every namespace entry that names that object.

Format 0.4 stores the index in one block. Namespace objects and hidden checksum objects share this prototype index, so large files also consume index entries through their checksum chains. A later tree-based index will remove this deliberate scaling limit.

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

Names are limited to 255 bytes in format 0.4. NUL and `/` are invalid in stored names. `.` and `..` are synthesized by the VFS/FUSE layer and are not stored as directory entries.

A format-0.4 directory occupies one metadata block. This is a prototype scalability limit, not a long-term design rule.

## 9. Regular files and extents

A regular-file payload contains common attributes, an extent count, the selected data-checksum algorithm, a 128-bit checksum-chain head object ID, and an ordered array of extent records.

The checksum head is zero for an empty file that has never allocated data. Once data blocks exist, it identifies the first hidden checksum object for the file.

Each extent stores:

```text
logical_block           first logical file block
physical_block          first physical filesystem block
block_count             number of consecutive blocks
flags                    extent encoding/policy flags
```
