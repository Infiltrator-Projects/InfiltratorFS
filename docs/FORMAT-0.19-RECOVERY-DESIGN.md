<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Format 0.19 — independently discoverable checkpoint locations

**Status: implemented and ready for testing in software 0.19.0.** Normal formatting, portable/native writable recovery, ordered checkpoint publication, grow/shrink relocation, writable repair, structural fsck and deep scrub use Format 0.19. Portable interruption/tear tests and mounted native VM tests exercise the actual implementation. Physical-media power-loss/endurance and PERF-019 measurements remain separate operator-run qualification work.

The authoritative checkpoint byte layout is `include/infilfs/format.h`; locator encoding is `include/infilfs/locator.h`. Both writers execute the ordering in `include/infilfs/checkpoint_protocol.h`. Current development accepts Format 0.19 only; it does not migrate or reinterpret Format 0.18 media.

## Problem and preserved commitments

Format 0.18 places three CRC64-protected authoritative CoW checkpoints at
block zero, near the original midpoint and near the original end. Grow/shrink
can preserve or relocate the secondary positions. If block zero is corrupt,
fallback to the *current physical* midpoint/end can miss surviving copies
despite their correct CRCs.

Format 0.19 retains three widely separated *actual* checkpoints and their
generation-based commit/recovery semantics. Locator records add discovery;
they do not replace checkpoints, do not confer data integrity, and are not
the commit marker.

## Persistent layout

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
The authoritative canonical header defines an 8-byte distinct magic,
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
2. Build a finite candidate set from the checkpoint offsets declared by
   structurally valid Format 0.19 locators and an independent structurally
   valid block-zero checkpoint. Reuse an existing map when the primary
   geometry already appears in a locator; otherwise retain its three
   candidates as well. Map contexts may share physical addresses. Reject
   positions outside the backing device and bound attempts to nine candidates.
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

The shared protocol preserves a recoverable committed graph at each modeled interruption point. The implemented sequence for **changing checkpoint locations** is:

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

`tests/format-019-durability.c` injects errors into the actual portable writer at every write/flush boundary, including torn writes and flushes that persisted before reporting an error. Recovery reopens cold storage, verifies the complete graph and file data, repairs writable replicas and resumes mutation. The independent crash model remains a separate ordering check. `tests/native-019-qualification.sh` exercises native writes, remounts, repeated relocation, physical backing-file shrink and recovery/repair after bootstrap damage. See `QUALIFICATION.md` for the evidence boundary.

Ordinary transactions that do *not* move checkpoint locations need no
locator rewrite; their selected checkpoint publication still follows the
native/portable durability protocol. Any ordinary `fsync` that rewrites a
fixed locator is a correctness/performance regression, and must be tested
explicitly. PERF-019 separately tracks physical writeback, `sync -f` latency,
write amplification and durability stage attribution; it does not authorise
reducing the recovery ordering barriers.

## Qualification boundaries

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
- The testing release requires the affected formatter/recovery/resize/fsck/native paths to pass together under the exact-source automatic release gate. Physical-media endurance, real power cuts and performance are additional production-deployment qualifications; no VM or storage model result substitutes for them.

## Rejected shortcuts

- Selecting current device midpoint/end after block-zero loss, despite resize.
- Grouping all authoritative checkpoints at fixed beginning-of-device blocks,
  thereby sacrificing the current distribution of checkpoint copies.
- Accepting a locator CRC or high generation as the committed truth.
- Treating a full multi-terabyte sector scan as ordinary fast-mount recovery.
- Writing new anchors into block numbers that may be owned by Format 0.18.
