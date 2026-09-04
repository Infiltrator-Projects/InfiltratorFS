<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Architecture

This document describes the architectural model and design invariants of InfiltratorFS. It deliberately does **not** track feature completion, workflow results or release history. Current completion belongs in `ROADMAP.md`; exact qualification evidence belongs in `QUALIFICATION.md`; persistent field/layout details belong in `ON_DISK_FORMAT.md`.

## 1. Architectural boundary

InfiltratorFS is defined by its persistent format and portable core, not by Linux VFS semantics or any other operating system.

The common layer owns:

- persistent filesystem and object identity;
- transactions and generations;
- checkpoints and recovery;
- allocation ownership;
- object, directory, extent, snapshot and integrity structures;
- portable attributes and extension points; and
- storage/durability/randomness/clock service contracts.

Operating-system adapters translate native APIs into that model. Equivalent concepts should map to one portable representation. Truly platform-specific semantics may be preserved in isolated adapter metadata rather than forcing every platform to imitate them.

## 2. Persistent identity and namespace

Filesystem and object identities are 128-bit persistent values. Paths are namespace mappings, not object identity.

Directories map UTF-8 names to persistent objects. Regular files may have multiple namespace references through hard links. Symbolic links are their own persistent objects with stored targets. Namespace operations are transactional and must preserve exact parent/reference/link-count invariants.

Format 0.17 uses 1023-byte UTF-8 component names, byte-exact case-sensitive comparison and explicit validation of forbidden traversal/reserved forms. Future Unicode normalization or case-folding policy must be versioned rather than silently changing current semantics.

## 3. Transactions, generations and checkpoints

Mutation is copy-on-write and generation based. A transaction constructs replacement state and publishes it through a new committed checkpoint generation.

Three physically separated checkpoints provide independent recovery candidates. Their committed locations are part of filesystem geometry; they are not blindly recomputed from current backing-device size.

The commit boundary is fail-closed:

1. replacement data/metadata and allocation state are written;
2. required durability operations complete;
3. the first new checkpoint is durably published, making the new generation authoritative;
4. remaining checkpoint replicas may then be refreshed.

If durability of the first publication cannot be established, further mutation must not continue as though the transaction definitely committed.

Recovery selects the newest complete structurally valid committed graph. Structural corruption can justify fallback to an older committed generation; unrelated I/O, unsupported-format or resource failures must not be misclassified as corruption.

Operation-level savepoints allow one failed operation to roll back its own tentative changes without discarding earlier acknowledged buffered work.

## 4. Allocation model

Allocation ownership is logically a one-bit-per-block model, persisted through a sharded copy-on-write allocation tree rather than a monolithic bitmap image. Runtime free-extent indexes are derived accelerators and can be rebuilt from authoritative allocation state.

This preserves the simplicity of bitmap ownership while allowing scalable publication: only affected allocation leaves and their replacement branch paths need to be rewritten.

Linux additionally uses volatile sharded reservation state so independent writers can search and reserve candidate data runs before the serialized metadata transaction. Reservation state is never authoritative persistent allocation.

Placement policy is deliberately volatile where possible. Sequential, random/in-place and sparse workloads may select different free runs; rotational and non-rotational media may score those runs differently. These choices must not redefine persistent object identity or Format 0.17 semantics.

Unsupported zoned-device write-pointer semantics are rejected rather than approximated unsafely.

## 5. File data and extents

Regular files may be empty, inline, extent-backed, sparse or reflinked.

Allocated file ranges are represented by extents; holes represent logical zero ranges without owning physical data blocks. Highly fragmented files may use paged extent metadata.

Shared extents implement reflinks. Mutation of shared data follows copy-on-write rules so one logical file update cannot modify another file or retained generation that still references the old physical data.

Logical file integrity is SHA-256 protected. Metadata structures are CRC64-ECMA protected.

## 6. Compression

Compression is per extent and bounded. The default native codec is IAC1 v1. Compression is selected only when it saves filesystem blocks; incompressible data falls back to ordinary extents rather than paying permanent expansion cost.

Compressed data remains ordinary logical file data from the namespace/API perspective. Partial mutation, truncate, hole punching, page-cache reads and defragmentation must preserve logical contents and integrity regardless of physical compressed representation.

The detailed codec/extent contract lives in `COMPRESSION.md`.

## 7. Snapshots and retained generations

Named snapshots identify immutable earlier generations. Blocks referenced by any retained generation remain unavailable for reuse until no live or retained graph references them.

Snapshot semantics are a consequence of the common generation/CoW model rather than a separate storage mechanism.

Current geometry-change design is conservative: resize may reject retained snapshots instead of attempting snapshot-aware geometry migration. Future snapshot rollback/restore should reuse the same retained-generation model rather than inventing an unrelated backup format.

## 8. Integrity, scrub and forensics

Normal reads validate the integrity metadata required for the objects/data they consume. Full scrub validates the reachable live graph plus retained snapshot state, including ownership and namespace invariants.

Metadata graphs must reject cycles or multiply aliased metadata pages where the format requires a tree. Traversal algorithms must be bounded in memory/stack behaviour and must fail closed on malformed topology.

The forensic scanner is deliberately separate from authoritative recovery. It can discover independently recognizable authenticated metadata from physical media, but discovery alone does not make a structure the committed filesystem state. See `FORENSICS.md`.

## 9. Administration

Online resize separates committed filesystem geometry from physical backing capacity. Grow/shrink rebuilds required allocation geometry transactionally, preserves or relocates checkpoint positions as required and refuses shrink that would discard live allocation.

Online defragmentation is copy-on-write. It may improve physical layout but must preserve object identity, logical content, hard-link identity, xattrs and user-visible timestamps while respecting snapshot/reflink retention.

Quota accounting is an adapter-level administrative policy over persistent objects rather than part of portable object identity. Linux quota state may persist rules/roots and rebuild usage from authoritative namespace/object state. Whether a quota implementation is currently complete belongs in `ROADMAP.md`, not here.

A future repair-capable fsck must distinguish deterministic repairs from cases where correctness cannot be established. Ambiguous corruption remains fail-closed.

## 10. Linux adapter

Linux uses the native out-of-tree `infiltratorfs.ko` VFS driver. The current architecture has no userspace FUSE fallback.

The adapter maps VFS inodes/dentries/page-cache operations onto persistent InfiltratorFS objects and transactions. Linux-only metadata such as POSIX ownership/mode, xattr namespaces and special-node details is isolated from the portable object model so it does not become the permanent cross-platform security definition.

The standard `mount.infiltratorfs` helper and InfiltratorFS Manager both request native `infiltratorfs` mounts. Packaging uses DKMS so the module is built against installed host kernel headers.

Kernel locking and transaction ownership must remain explicit. As the driver grows, lock-order and deferred-publication rules should be centralized rather than inferred from scattered call paths.

## 11. Windows adapter

Windows currently has portable-core image/raw-device access and a user-mode ProjFS Explorer bridge. The bridge projects InfiltratorFS content through an NTFS virtualization root and persists supported Windows mutations back through the portable core.

ProjFS is an interoperability bridge, not a native filesystem driver. A future native Windows adapter must integrate with the Windows I/O Manager, Cache Manager, Memory Manager, security descriptors, share/delete semantics, reparse behaviour and native volume mounting while preserving the same portable persistent model.

## 12. Security model

Current POSIX compatibility fields and Linux xattr sidecars are not the final portable security authority.

The intended long-term model uses versioned portable security objects, stable typed principals, portable rights and explicit Linux/Windows mapping. Platform-specific security metadata must be preservable even when another adapter cannot interpret it. See `SECURITY.md`.

## 13. Design guardrails

- Persistent format semantics must not depend on one operating system's API names.
- Runtime policy should remain replaceable and non-persistent unless persistence is required for correctness.
- The single-device filesystem must remain complete and understandable without multi-device features.
- New persistent hard limits require explicit rationale and scale analysis.
- Compression, encryption, protection classes, replication, case-folding and named streams should build on existing primitives instead of creating parallel incompatible models.
- Fail closed when integrity, durability or recovery correctness cannot be established.

For field-level Format 0.17 details, use `ON_DISK_FORMAT.md`. For what is finished, use `ROADMAP.md`. For proof, use `QUALIFICATION.md`.