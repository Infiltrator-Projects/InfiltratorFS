<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS On-Disk Format 0.4 — Part 1

Status: experimental writable prototype. The format is not frozen and is intentionally incompatible with the earlier 0.1, 0.2 and 0.3 prototypes.

## 1. Encoding and allocation unit

All integer fields are little-endian. The format-0.4 filesystem block size is fixed at 4096 bytes and the superblock records a block shift of 12.

Metadata objects occupy one filesystem block in format 0.4. File data is allocated in 4096-byte blocks described by extents.

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
format major/minor      current format is 0.4
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

The bitmap is authoritative. On open, format 0.4 recounts free blocks and rejects the volume if the bitmap disagrees with the committed `free_blocks` value. Bitmap storage blocks, checkpoints, the object index and root object must all be marked allocated.

Future free-extent indexes are accelerators only and must remain rebuildable from this bitmap.

## 5. Metadata object header

All metadata objects begin with `struct infs_object_header_disk`:

```text
magic                   8 bytes: "INFOBJ01"
object type             directory, regular file, object index or checksum object
object version          type-specific version
header size             common-header size
generation              most recent object update generation
object ID               128-bit persistent identity
parent ID               recovery/namespace relationship for files/directories
payload size            bytes of type-specific payload
checksum type           CRC64-ECMA
checksum                 32-byte reserved checksum field
```
