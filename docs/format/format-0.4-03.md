<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS On-Disk Format 0.4 — Part 3

Format 0.4 implements normal extents only. The logical extent sequence must be contiguous from logical block zero; sparse extents are deferred. Writes beyond EOF therefore allocate the intervening blocks and initialize unwritten bytes to zero.

The allocator searches the authoritative bitmap for contiguous free runs. Adjacent logical and physical allocations are coalesced into the previous extent when possible. Multiple extents are supported when fragmentation prevents one contiguous run.

Shrinking a file releases blocks beyond the new EOF and copy-on-write zeroes the unused tail of the retained final block so a later regrow cannot expose previously truncated data.

## 10. Data checksum objects

Each allocated logical file block has one independently stored checksum. Checksum metadata is deliberately separate from the physical data extent so corruption of a data block cannot silently modify the only checksum used to authenticate it.

Checksum objects use the common metadata header with object type `INFS_OBJECT_CHECKSUM`. Their parent ID and payload owner ID identify the regular file whose logical blocks they authenticate. The payload contains:

```text
owner_object_id         128-bit regular-file identity
next_object_id          next checksum object in logical order, or zero
start_logical_block     first logical data block covered by this object
checksum_count          number of populated checksum entries
reserved                reserved
checksum entries        fixed 32-byte slots
```

A checksum object's start block is aligned to `INFS_CHECKSUMS_PER_OBJECT`, and the chain advances in fixed logical-block ranges. The current 4096-byte object layout stores at least 120 checksums per object.

Each data-checksum slot is 32 bytes. Format 0.4 selects SHA-256 for file data and stores the complete 256-bit digest in that slot. The file payload stores the checksum algorithm ID so later formats can introduce alternative algorithms without changing the checksum-object record shape.

The checksum is calculated over the complete 4096-byte physical data block, including zero-filled bytes beyond logical EOF in the final allocated block. Normal reads verify the checksum before returning any portion of that block. `infilfs-scrub` walks every allocated regular-file block and reports checksum mismatches without attempting repair.

## 11. Namespace operations

Format 0.4 supports:

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

## 12. Transaction and checkpoint publication

Format 0.4 assigns each mutating filesystem operation to a transaction based on committed generation `N`. Metadata objects changed by the operation are never rewritten in their committed physical blocks. Replacement objects receive generation `N+1`, are checksummed, and are written to newly reserved blocks. The object index is also copy-on-write, so namespace objects can move without changing their persistent 128-bit IDs.

The authoritative bitmap is also copy-on-write. Blocks allocated while constructing `N+1` are not reachable from any durable checkpoint until commit. Blocks superseded or deleted by the operation remain reserved while the transaction is being constructed and are marked free only in the new bitmap image. This prevents the allocator from reusing storage still required by the old committed generation before the commit point.
