<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Format 0.19 — independently discoverable checkpoint locations

**Status: design accepted; portable and native locator codecs implemented as non-active helpers, but no complete 0.19 formatter/mount/resize implementation or qualification.**
Until a complete native/portable implementation and exact-source validation
land together, the only shipping persistent format remains Format 0.18.
The authoritative current Format 0.18 byte layout remains `include/infilfs/format.h`. The proposed Format 0.19 locator schema, bounded candidate-list builder and initial allocation geometry planner are defined by `include/infilfs/locator.h` and `src/locator.c` (not yet active in mounted filesystems).

## Problem and preserved commitments

Format 0.18 places three CRC64-protected authoritative CoW checkpoints at
block zero, near the original midpoint and near the original end. Grow/shrink
can preserve or relocate the secondary positions. If block zero is corrupt,
fallback to the *current physical* midpoint/end can miss surviving copies
despite their correct CRCs.

Format 0.19 must retain three widely separated *actual* checkpoints and their
generation-based commit/recovery semantics. Locator records add discovery;
they do not replace checkpoints, do not confer data integrity, and are not
the commit marker.

## Persistent layout proposal

- Minimum formatted filesystem remains 16 MiB with 4096-byte blocks.
- Two **permanent locator blocks** have zero-based physical block addresses
  **1** and **2047** (8 MiB minus 4 KiB). These addresses are constant for
  every Format 0.19 volume and remain within every permitted shrink target.
- The formatter marks both locator blocks allocated and unavailable to all
  normal data/metadata and snapshot allocation. Allocation metadata initial
  placement must *skip* these blocks, rather than overwriting them or
  assuming contiguous placement. Scrub treats them as known reserved
  geometry, not lost objects, and validates both independently.
- Three authoritative checkpoint blocks retain initial positions at
  **0**, **floor(N/2)** and **N-1**, with committed positions described in
  each checkpoint. Their second and third addresses can change under resize
  without changing the two locator positions. For N >= 4096 neither locator
  initially aliases a checkpoint.
- Existing Format 0.18 superblocks, allocation maps or spare/padding bytes
  MUST NOT be reinterpreted as 0.19 locators. New Format 0.19 is a strict,
  incompatible minor version, not a hidden optional compatible feature.

Each locator is one zero-padded, fixed-width, little-endian 4096-byte record.
The eventual authoritative packed header MUST define an 8-byte distinct magic,
locator schema version, exact header size, Format 0.19 identity, volume UUID,
the *hinted* committed generation, committed filesystem block count, exactly
three checkpoint physical offsets, and a CRC64-ECMA of the canonical page
with its checksum field zeroed. Reserved bytes are canonical zero; impossible
geometry, duplicate/out-of-range offsets and unsupported schemas are invalid.
The two copies have independent checksums and may legally describe different
generations during interrupted publication.

Locator values are **untrusted hints**. Neither a valid locator CRC nor a
larger generation number makes a candidate checkpoint authoritative.
Every candidate must independently pass superblock CRC/format/UUID/location
consistency, feature checks, allocation-tree and complete committed-graph
validation. Stale, corrupt or contradictory locator pages must not cause
unvalidated writes, invented checkpoint addresses, unbounded scanning, or
silent rollback on an external read error.

## Recovery algorithm

1. Attempt block zero, and read both fixed locators independently.
2. Build a finite, deduplicated candidate-address set from the checkpoint
   offsets declared by structurally valid Format 0.19 locators and from a
   structurally valid block-zero checkpoint. Reject positions outside the
   actual backing device. Bound all attempts even under attacker-controlled
   checksummed bytes.
3. Read independent checkpoint candidates, require their internally recorded
   positions to agree with the source locator being used, and prefer the
   newest **fully validated, durably committed** generation. Different
   locator generations are expected around a crash; unsupported format,
   I/O and resource errors are not silently called structural corruption.
4. On read-only recovery, never heal locator/checkpoint media. Writable
   healing is allowed only after a committed graph is selected and explicit
   ordering/rollback conditions prove safe.
5. If *all* bootstrap positions fail, report an unrecoverable or explicit
   offline-forensic condition rather than pretending random physical scans
   are constant-time normal mount.

Neither logical separation nor three CRCs guarantees recovery from every
possible simultaneous media failure, malicious rewrite or unsupported device
durability behaviour.

## Resize and crash-ordering obligations

The protocol needs an invariant strong enough to cover *every* interruption
point, not just a successful final result. A candidate sequence for **changing
checkpoint locations** is:

1. Keep all old committed checkpoints/CoW dependencies reachable. Construct
   and durably flush the next allocation graph and all replacement records.
2. Write and durably flush locator A as a *hint* for the proposed new
   checkpoint positions. Locator B still describes the old positions.
   The new locator alone is **not** a commit.
3. Publish and durably flush the selected new-generation checkpoint at the
   designated commit boundary, without destroying the final recoverable old
   checkpoint before that boundary.
4. Write and **durably flush each secondary checkpoint individually** at its
   new position. Never submit both secondary replacements together while a
   corrupted or torn block-zero checkpoint could make every backup unusable.
   Only then update and durably flush locator B. Do not report successful
   resize/permit backing-partition reduction before this completes.
5. Keep storage referenced by either still-authoritative old checkpoint
   protected until the recovery protocol no longer requires it. Any
   ambiguous publication/locator durability failure must poison or suspend
   further writes rather than claiming a clean success.

**This is a proposed write ordering, not yet a proven algorithm.** The
implementation must explicitly model all pending writes and candidate
generations and prove that no crash window can leave locators pointing only
to unreachable/deallocated state. A validated unit/fault model that fails
at every flush, checkpoint and locator write is an implementation prerequisite.

Ordinary transactions that do *not* move checkpoint locations need no
locator rewrite; their selected checkpoint publication still follows the
native/portable durability protocol.

## Proof and release gates

- New formatter creates Format 0.19 and reserves correct locator blocks;
  existing Format 0.18 is rejected rather than reformatted on mount.
- Portable and native readers accept the **same** canonical locator fixtures,
  reject malformed versions/CRC/positions, and agree on candidate selection.
- Both native and portable resize can grow and shrink repeatedly, including
  remote checkpoint relocation; corrupt block zero afterwards and remount
  successfully using fixed locators, then deep scrub to CLEAN.
- Fault injection interrupts and corrupts each individual locator write,
  each checkpoint write and each flush. It must demonstrate recoverability
  of at least a previously committed complete graph, not merely a bootable
  header or successful CRC parse. Test damaged media, stale and divergent
  locators, UUID mismatch, device size disagreement, and interrupted shrink.
- Dedicated media qualification captures actual kernel, format identity,
  hardware, mount options and full results. Tests must not run destructively
  on an unconfirmed device. In particular no part of this decision authorizes
  formatting `/dev/nvme0n1p5`.
- The change is released as a new minor *disk* format only **after** all
  affected formatter/recovery/resize/fsck/native paths pass together. Never
  increment `INFS_FORMAT_MINOR` in isolation and publish an unqualified
  version with incompatible media.

## Rejected shortcuts

- Selecting current device midpoint/end after block-zero loss, despite resize.
- Grouping all authoritative checkpoints at fixed beginning-of-device blocks,
  thereby sacrificing the current distribution of checkpoint copies.
- Accepting a locator CRC or high generation as the committed truth.
- Treating a full multi-terabyte sector scan as ordinary fast-mount recovery.
- Writing new anchors into block numbers that may be owned by Format 0.18.
