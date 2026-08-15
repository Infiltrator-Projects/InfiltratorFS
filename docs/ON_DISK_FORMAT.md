<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS On-Disk Format 0.1

Status: experimental prototype. The format is not frozen.

## 1. Byte order and basic unit

All integer fields are little-endian.

The format 0.1 filesystem block size is fixed at 4096 bytes. Future format revisions may allow other block sizes, but the superblock records the block-size shift explicitly.

## 2. Volume layout

For a volume containing `N` filesystem blocks:

```text
block 0                 checkpoint/superblock A
block 1 .. B            allocation bitmap
block B+1               root directory object
...
block floor(N/2)        checkpoint/superblock B
...
block N-1               checkpoint/superblock C
```

The bitmap is large enough to contain one bit for every filesystem block. Bits outside the actual volume range in the final bitmap block are permanently marked unavailable.

All checkpoint blocks, bitmap blocks and the root-object block are themselves marked allocated.

## 3. Superblock/checkpoint

The current disk structure is defined by `struct infs_superblock_disk` in `include/infilfs/format.h`.

Conceptual fields:

```text
magic                   8 bytes: "INFS2026"
format major/minor      version of the on-disk format
header size             structure-size guard
block shift             12 for 4096-byte blocks
checksum type           CRC64-ECMA in format 0.1
generation              committed generation number
total blocks            volume size in filesystem blocks
free blocks             committed free-block count
bitmap start/count      location of authoritative allocation bitmap
root object block       physical location of current root object
checkpoint blocks[3]    expected checkpoint positions
filesystem UUID         128-bit unique identifier
root object ID          128-bit persistent root identity
feature flag sets       compat / read-only compat / incompatible
label                   up to 63 bytes plus terminator in prototype tools
checksum field          32 bytes reserved for checksum data
```

Format 0.1 writes a CRC64-ECMA value into the first eight bytes of the 32-byte checksum field and leaves the remainder zero. During checksum calculation the complete checksum field is treated as zero.

## 4. Checkpoint validation

A checkpoint is valid only when all of the following hold:

- magic matches;
- supported major format version;
- header size matches the expected structure;
- block size is supported;
- checksum algorithm is supported;
- checksum validates;
- `total_blocks` matches the size derived from the containing image or block device.

The current reader checks the three physically derived candidate locations and chooses the valid copy with the greatest generation number.

Future transaction logic will add cross-checks among root generations, bitmap generations and checkpoint publication state.

## 5. Allocation bitmap

Bit `b` describes filesystem block `b`.

Format 0.1 uses:

```text
0 = free
1 = allocated or unavailable
```

The bitmap is the exact allocation source of truth. Future extent-oriented free-space indexes must be rebuildable from it.

## 6. Object blocks

Metadata objects use a common header beginning with:

```text
magic                   8 bytes: "INFOBJ01"
object type             e.g. directory
object version          type-specific format version
header size             structure-size guard
generation              object generation
object ID                128-bit persistent identifier
parent ID                optional/recovery relationship hint
payload size             valid type-specific payload bytes
checksum type            CRC64-ECMA in format 0.1
checksum                 32-byte reserved checksum field
```

As with superblocks, CRC64 occupies the first eight checksum bytes and the checksum field is zeroed while calculating it.

## 7. Root directory object

Format 0.1 creates one directory object with:

- the 128-bit root object ID recorded by the superblock;
- object generation 1;
- zero parent ID;
- directory entry count 0.

Directory entries are intentionally not yet encoded. The next format step will introduce a variable-length directory record format and persistent object lookup.

## 8. Feature flags

Three 64-bit feature sets are reserved:

- compatible: older readers may safely ignore an unknown bit;
- read-only compatible: unknown bits allow read-only mounting only;
- incompatible: unknown bits prohibit mounting.

No feature flags are assigned in format 0.1.

## 9. Safety rules for writers

A future writable implementation must not silently update structures in a way that breaks older committed generations. Metadata publication is generation-based and transactional.

Any structure change that cannot be safely ignored by older implementations must be represented through format versioning or incompatible feature flags.
