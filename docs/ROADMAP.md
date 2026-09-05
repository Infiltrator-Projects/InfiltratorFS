<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Roadmap

This file is the **single authoritative feature-completion list** for InfiltratorFS.

A checked item means the capability is implemented **and** has the qualification required for that capability. An unchecked item may already have substantial code in `main`, but it is not complete until its required qualification passes. Exact-source evidence belongs only in `QUALIFICATION.md`.

Pre-1.0 development is current-format-only. A development format may be replaced without a backward reader or migration path; stable compatibility begins with the first stable release.

## Core filesystem foundation

- [x] 128-bit persistent filesystem and object identities.
- [x] Three physically separated checkpoints and generation-based recovery.
- [x] Copy-on-write transactions with atomic checkpoint publication.
- [x] Authoritative one-bit-per-block allocation persisted as a scalable sharded allocation tree.
- [x] Rebuildable runtime free-extent index.
- [x] CRC64 metadata integrity and SHA-256 logical file-data integrity.
- [x] Portable C17 core with explicit storage/durability/randomness/clock services.
- [x] Stable platform-neutral status/error contract.
- [x] Malformed-image, crash/recovery and integrity rejection coverage.

## Namespace and file model

- [x] 1023-byte UTF-8 namespace components.
- [x] Generation-aware scalable object-index and directory trees.
- [x] Inline small files.
- [x] Sparse files and hole extents.
- [x] Shared extents and reflinks.
- [x] Paged extent metadata.
- [x] Symbolic links.
- [x] Regular-file hard links.
- [x] Named read-only snapshots and retained historical generations.
- [x] Portable attributes with POSIX compatibility metadata isolated from the portable model.
- [ ] Versioned Unicode normalization policy.
- [ ] Optional case-folded directory policy.
- [ ] Cross-platform removable-volume filename profile.
- [ ] Generic typed/reparse extension objects.
- [ ] Generic named streams/extended metadata model.

## Allocation, performance and storage policy

- [x] Sequential-allocation and checksum hot-path optimization.
- [x] Parallel native allocation reservations.
- [x] Locality scoring and workload-aware placement.
- [x] Media-aware placement with rotational/non-rotational policy.
- [x] Mounted million-file and 1 TiB scale qualification.
- [x] Near-full fragmentation/refill and mixed-workload endurance qualification.
- [x] Native fragmentation metrics.
- [x] Bounded online copy-on-write defragmentation.
- [x] Adaptive per-extent compression using native IAC1 v1 with bounded memory and incompressible fallback.

## Native Linux filesystem

- [x] Native out-of-tree VFS driver `infiltratorfs.ko`.
- [x] DKMS package/build/install path.
- [x] Native read-only and read-write mounting of current Format 0.17.
- [x] Create/mkdir/mknod, rename/unlink/rmdir and persistent setattr.
- [x] Random, sparse and high-offset writes; truncate and `fallocate`.
- [x] Hole punching and logical/allocated block reporting.
- [x] Reflinks and Linux `FICLONE`/`remap_file_range` path.
- [x] Hard links and symbolic links.
- [x] Persistent Linux xattr namespaces.
- [x] FIFO/socket/character/block special-node identity.
- [x] Page cache, readahead and shared writable `mmap`.
- [x] Crash-safe open-unlink/replacement lifetime and mount-time orphan recovery.
- [x] Writable live generations while snapshots retain older generations.
- [x] Dirty-checkpoint fallback and writable checkpoint replica healing.
- [x] Metadata-tree alias/cycle rejection and kernel-stack hardening.
- [x] Multi-process namespace/data/xattr/open-unlink contention qualification.
- [x] Native mount helper and Manager native-mount integration.
- [x] udev/UDisks identification and desktop visibility.
- [x] Native-only packaging with no FUSE runtime fallback.
- [x] libblockdev/UDisks/GNOME Disks formatter integration implementation and qualification.

## Administration and recovery

- [x] Online filesystem grow and safely bounded shrink.
- [x] Native user/group/project quotas with durable policy and remount usage reconstruction.
- [ ] Deterministic repair-capable filesystem checker for unambiguous repair cases.
- [ ] Snapshot restore/rollback for selected objects and whole-volume recovery to a retained generation.

## Security and protection

- [ ] Versioned portable security objects with stable typed principals and ACL entries.
- [ ] Generic access-right vocabulary independent of POSIX and Windows constants.
- [ ] Linux UID/GID/mode and POSIX ACL mapping policy.
- [ ] Windows SID/security-descriptor mapping policy.
- [ ] Preservation rules for platform-specific security metadata.
- [ ] Portable named-attribute/named-stream objects distinct from adapter sidecars.
- [ ] Protection classes and multi-device placement.
- [ ] Replication and/or parity.
- [ ] Encryption domains and key wrapping.
- [ ] Authenticated metadata for encrypted volumes.

The intended security architecture is described in `SECURITY.md`. Current Linux ownership/mode/xattr metadata must not be mistaken for the final portable security model.

## Operating-system adapters

- [x] Native Linux VFS adapter as the current mounted reference implementation.
- [x] Win32 image/raw-partition storage backend.
- [x] Windows formatter, transfer/listing and scrub application.
- [x] Linux/Windows Format 0.17 interoperability coverage.
- [x] Driverless Windows Explorer projection using Microsoft's inbox ProjFS.
- [ ] Windows attribute/security/filename adapter completion.
- [ ] Native Windows filesystem driver with Cache Manager/I/O Manager integration.
- [ ] macOS native adapter investigation/implementation.
- [ ] BSD native adapter investigation/implementation.
- [ ] Haiku native adapter investigation/implementation.

See `PLATFORM_ADAPTERS.md` for the adapter contract.

## Deliberately deferred

Global synchronous deduplication, distributed/network filesystem semantics and application-visible transactions are not first-generation requirements.

## Design guardrails

The roadmap should remain focused on broadly useful filesystem capabilities. Persistent hard limits and format choices require explicit rationale; runtime policy such as placement should stay non-persistent where possible. The single-device filesystem must remain complete in its own right, while multi-device protection, replication and encryption remain optional layers rather than distortions of the core design.
