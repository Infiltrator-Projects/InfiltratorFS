# Decisions

This file records durable architectural decisions for InfiltratorFS. Detailed format contracts remain in `ON_DISK_FORMAT.md`; exact qualification evidence remains in `QUALIFICATION.md`.

## ADR-001 — One portable filesystem semantics, multiple OS adapters

**Decision.** The persistent format and portable core define filesystem meaning; Linux, Windows and future adapters translate native APIs onto that model.

**Rationale.** The filesystem should not become "the Linux implementation" with other ports reconstructing semantics independently.

**Consequence.** OS-specific metadata is isolated at adapter boundaries and format changes are platform-neutral decisions.

## ADR-002 — Mutation is generation-based copy-on-write

**Decision.** Transactions construct replacement state and publish it through a committed generation/checkpoint rather than overwriting the current graph in place.

**Rationale.** Atomic publication and recovery are easier to reason about when incomplete replacement state is not yet authoritative.

**Consequence.** Allocation/reference accounting must remain generation-aware and crash recovery selects a valid committed graph.

## ADR-003 — Three physically separated checkpoints

**Decision.** Current Format 0.19 maintains three independently validated checkpoint copies.

**Rationale.** Small deliberate metadata redundancy improves recoverability from torn/corrupt primary metadata.

**Consequence.** Checkpoint validation includes position/format/checksum consistency before a referenced graph is trusted.

## ADR-004 — 128-bit filesystem/object identity

**Decision.** Persistent filesystem and object identities use 128-bit values.

**Rationale.** Identity should not depend on host inode widths or one operating system's namespace assumptions.

**Consequence.** Adapters map native identifiers to persistent objects rather than defining object identity themselves.

## ADR-005 — History/snapshots are first-class generation semantics

**Decision.** Retained historical generations, snapshots and reflinks participate in reference ownership and block-reuse safety.

**Rationale.** Version history cannot be bolted on after allocation because old generations must prevent premature reuse.

**Consequence.** Reclamation paths must prove a block is unreferenced by live files, reflinks and retained generations before freeing it.

## ADR-006 — Structural fsck and deep scrub are distinct costs

**Decision.** Normal fsck validates structural/accounting integrity; exhaustive payload/history verification is an explicit scrub mode.

**Rationale.** A routine structural check should not scale like reading every data byte on a large healthy filesystem.

**Consequence.** Documentation/tests must not claim a fast fsck has performed deep data-integrity verification.

## ADR-007 — Native Linux is the normal mounted path

**Decision.** The Linux product uses the native VFS/DKMS driver; there is no FUSE runtime fallback.

**Rationale.** Native page-cache, mmap, writeback and filesystem integration are core product requirements.

**Consequence.** Mounted qualification is a separate evidence class from portable userspace tests.

## ADR-008 — Compression policy remains filesystem-owned

**Decision.** InfiltratorFS owns its compression semantics and native IAC1 format; LZ4 remains a retained non-default reference/interoperability representation.

**Rationale.** Compression affects persistent extents, qualification and performance and therefore belongs to the filesystem contract.

**Consequence.** Generic compression-library convenience does not override format ownership.

## ADR-009 — Backup checkpoint discovery after resize (resolved by Format 0.19)

**Status:** Implemented and qualified for testing in Format 0.19. Physical-media power-loss/endurance and performance qualification remain open; see `ROADMAP.md` and `QUALIFICATION.md` for their separate evidence classes.

The following records the original Format 0.18 limitation that motivated this decision.

Block zero records committed secondary checkpoint positions. Online grow/shrink
can preserve or relocate those positions. If block zero is invalid, both the
native and portable Format 0.18 openers fall back to physical midpoint/end
locations; those positions need not contain valid backups after resize.

The replacement format was required to make backup locator recovery independent of block
zero and physical capacity, with bounded discovery cost, correct recovery
across geometry changes, torn checkpoint writes and power loss. Do not assign
a locator into an existing occupied Format 0.18 block or silently change the
current disk contract. An explicit offline media scan can be a rescue option,
but cannot masquerade as normal rapid mount recovery.

**Qualification:** Repeated grow/shrink with secondary relocation; corrupt
block zero; recover and deep scrub the newest complete committed generation
using surviving locators alone. Include interrupted geometry publication and
independently corrupted or relocated replicas.

## ADR-010 — Independent checkpoint location anchors for Format 0.19

**Decision:** Retain three geographically separated authoritative checkpoints, but add two permanently located, checksummed *discovery-only* locator records. Their fixed positions and versioned representation are defined by `FORMAT-0.19-RECOVERY-DESIGN.md`. The locator is never an authoritative transaction commit and cannot authorize a generation on its own.

**Rationale:** Format 0.18 cannot always locate secondary checkpoints after a resize if block zero is lost. Moving the checkpoints to a compact fixed cluster would compromise the original three-copy placement objective. Two fixed, independently readable locator positions preserve three distant checkpoint locations without a device scan.

**Compatibility:** This requires a genuine new Format 0.19 layout and explicit, destructive reformat or a separately qualified migration. Format 0.18 media must not be silently reinterpreted or mutated by Format 0.19.

**Status:** Implemented and exercised by Format 0.19 portable interruption tests and native Linux mounted qualification for testing. This does not establish production physical-media durability, performance or mounted Windows-driver qualification; see `ROADMAP.md` and `QUALIFICATION.md`.
