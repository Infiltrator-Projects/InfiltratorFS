<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Roadmap

This file is the **single authoritative feature-completion list** for InfiltratorFS.

A checked item means the capability is implemented **and** has the qualification required for that capability. An unchecked item may already have substantial code in `main`, but it is not complete until its required qualification passes. Exact-source evidence belongs only in `QUALIFICATION.md`.

Pre-1.0 development is current-format-only. A development format may be replaced without a backward reader or migration path; stable compatibility begins with the first stable release.

## Format 0.19 recovery redesign

- [x] Two independent fixed-position, checksummed checkpoint-location records in the new on-disk format, with no locator authority over checkpoint graph validation.
- [x] Portable formatter, recovery, allocation accounting, resize and fsck/scrub agree on the new locator and reserved-block contract.
- [x] Native Linux mount, recovery and resize use the same locator schema and commit ordering, verified against portable media.
- [x] Deterministic crash/tear/corruption matrix: block-zero loss after grow/shrink, stale/divergent locators, interrupted publication, old-format rejection and remount/scrub.
- [x] Native mounted write/remount/grow/shrink, physical backing-image reduction, block-zero/locator loss and writable repair qualification for the Format 0.19 testing release.
- [ ] Operator-run physical-media power-loss/endurance qualification before production deployment; virtual-machine and injected-storage results do not establish hardware durability.
- [ ] PERF-019: instrument and attribute native Linux final-sync latency across VFS writeback, deferred CoW preparation, lock contention and durability publication without removing required crash-consistency barriers.
- [ ] PERF-019: demonstrate repeatable 0.19 physical-NVMe performance and post-remount integrity for 256 MiB sequential writes, the deterministic 1,460-file Windows tree, 4 KiB fsync latency distribution and metadata-heavy operations (see `PERF-019-SYNC-LATENCY.md`).

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
- [x] Versioned Unicode-name policy v1 (validated UTF-8 preserved byte-for-byte, without Unicode normalization).
- [x] Optional case-folded directory policy.
- [x] Cross-platform removable-volume filename profile.
- [x] Generic typed/reparse extension objects.
- [x] Generic named streams/extended metadata model.

## Allocation, performance and storage policy

- [x] Sequential-allocation and checksum hot-path optimization.
- [x] Parallel native allocation reservations.
- [x] Filesystem-wide native Linux CPU-heavy concurrency budget of `max(1, online physical cores - 1)`, with SMT siblings left as scheduler headroom/capacity, independent data/metadata work able to scale to that budget, and only short atomic publication coordination globally serialized.
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
- [x] Native read-only and read-write mounting of current Format 0.19.
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
- [x] Deterministic repair-capable filesystem checker for unambiguous repair cases.
- [x] Crash-safe whole-volume rollback to a retained snapshot generation.
- [x] Selected-object restore from a retained snapshot without rolling back the whole volume.

### Accidental-format resilience (new core capability)

- [ ] Compare candidate recovery architectures against real native and foreign formatter overwrite patterns; select the strongest design without requiring preservation of the pre-1.0 format or implementation.
- [ ] Implement common non-destructive format preflight and explicit target-bound authorisation across all formatter entry points, including Common V2.
- [ ] Design and implement distributed recovery anchors and redundant essential metadata with transactional publication, allocation accounting, resize handling and bounded overhead; introduce a new development format if appropriate.
- [ ] Implement recoverable native quick formatting with new volume identity, validated prior-graph retention, allocation/discard protection and explicitly authorised irreversible release; fail closed when safe retention is impossible.
- [ ] Implement independent offline historical graph reconstruction and separate-destination export, recovering verified names, hierarchy, identities, links, sparse/shared/compressed data, attributes, ACLs and snapshots where evidence survives; report missing and ambiguous data.
- [ ] Support best-effort recovery after foreign quick formatting and repartitioning; handle encrypted metadata, recoverable key headers, authorised access and purge without weakening security.
- [ ] Qualify interruption safety, foreign overwrite patterns, recovery fidelity, overhead and physical-media boundaries under the planned contract in `QUALIFICATION.md`.

Existing checkpoints and forensic scanning are potential building blocks, not architectural constraints or proof of this capability. All gates are unqualified.

## Security and protection

- [x] Versioned portable security objects with volume-level stable principals, multi-binding identity resolution, shareable descriptors and ordered ACL entries.
- [x] Generic access-right vocabulary independent of POSIX and Windows constants.
- [x] Linux UID/GID/mode and POSIX ACL mapping policy.
- [x] Windows SID/security-descriptor mapping policy.
- [x] Preservation rules for platform-specific security metadata.
- [x] Portable named-attribute/named-stream objects distinct from adapter sidecars.
- [x] Multi-device synchronous replicated storage with failover across complete replicas.
- [x] Per-object protection classes and deterministic placement policy across multiple devices.
- [x] Replication via synchronous complete-volume mirroring; parity remains a future optional protection mode.
- [ ] Persistent multi-device membership, stable placement slots, durable stale-member fencing, authoritative generation selection and interruption-safe resynchronisation. The current ordered storage wrapper detects observed replica disagreement and blocks further writes/flushes, but reopening is not repair. No automatic selection or overwrite of divergent members is permitted without a qualified recovery authority.
- [x] Authenticated whole-volume encryption with PBKDF2-derived KEK and wrapped random 256-bit volume key.
- [x] Multiple/per-object encryption domains derived from the authenticated volume root key.
- [x] Authenticated metadata and data for encrypted volumes through per-block AES-256-GCM.

The intended security architecture is described in `SECURITY.md`. Current Linux ownership/mode/xattr metadata must not be mistaken for the final portable security model.

## Operating-system adapters

- [x] Native Linux VFS adapter as the current mounted reference implementation.
- [x] Win32 image/raw-partition storage backend.
- [x] Windows formatter, transfer/listing and scrub application.
- [x] Linux/Windows Format 0.19 interoperability coverage.
- [x] Driverless Windows Explorer projection using Microsoft's inbox ProjFS.
- [x] Windows attribute/security/filename adapter completion.
- [x] Native Windows filesystem driver implementation with Cache Manager/I/O Manager integration.
- [ ] Mounted native Windows driver qualification on Windows (real volume mount, cached/non-cached/paging I/O, mmap coherence, namespace/security semantics, service restart, Driver Verifier and cross-platform conformance).
See `PLATFORM_ADAPTERS.md` for the adapter contract.

## Future ecosystem integration

Possible future integrations with Software, System Monitor, Defragmenter, Link-family applications, RunnerScope and other Infiltrator projects are recorded in `FUTURE_ECOSYSTEM_INTEGRATION.md`.

These are **not current feature-completion gates**. They remain subordinate to the present priority: make the native Linux filesystem boot, mount, recover and sustain ordinary desktop/server workloads with normal latency and throughput before adding ecosystem-specific enhancement layers.

The integration design is capability-based and optional: applications must continue to work on other filesystems, standard operating-system APIs are preferred where sufficient, and InfiltratorFS must not acquire product-specific knowledge such as Debian package or diagnostic-record semantics.

## Deliberately deferred

Global synchronous deduplication, distributed/network filesystem semantics and application-visible transactions are not first-generation requirements.

## Design guardrails

The roadmap should remain focused on broadly useful filesystem capabilities. Persistent hard limits and format choices require explicit rationale; runtime policy such as placement should stay non-persistent where possible. The single-device filesystem must remain complete in its own right, while multi-device protection, replication and encryption remain optional layers rather than distortions of the core design.

---

## Proposed Format 0.20 — unified design (revisions 3–4)

**Proposal only; the preceding current-format contract remains applicable to Format 0.19.** The [unified Format 0.20 specification](ON_DISK_FORMAT.md#proposed-infiltratorfs-on-disk-format-020) is the design authority for the proposed persistent model. This section specialises that model; it does not introduce a second on-disk authority or claim implementation.

All items below are **unchecked proposed 0.20 gates**. Existing 0.19 checkmarks retain their version-scoped meaning and are not inherited.

- [ ] Freeze coherent byte schema, canonical semantic membership and location commitments, bootstrap arena/reserve, strong-edge/GC model and publication state machine.
- [ ] Implement shared immutable allocation descriptors, location maps, indexed logical integrity, compatible reflinks and transactional relocation across retained roots.
- [ ] Implement canonical recoverable metadata placement, independent commit/discovery paths and bounded mount; measure actual foreign formatter write/discard/first-mount patterns before fixing the profile.
- [ ] Implement one portable ownership/transaction engine for objects, security, streams, quotas, open-orphans, snapshots, retired roots and deferred reclaim; validate NoCoW eligibility or use CoW.
- [ ] Implement common Unformatter, native quick-format retention/reversal, foreign reconstruction/export and controlled in-place restoration; zero-write refusal for unsafe targets.
- [ ] Integrate independent encrypted headers, key lifetime and allocation protection; implement persistent replica membership/authority/resync before claiming support for degraded writes.
- [ ] Integrate same schema and capability contracts in portable, native Linux, Windows and Common V2/BOOT paths; preserve all existing portable namespace and maintenance semantics.
- [ ] Establish actual 0.19/comparable filesystem workload baselines, measure memory, metadata overhead, clone/unlink/relocation cost, write amplification and fsync latency; pass new exact-source qualification and physical-media gates.
- [ ] Update active implementation version and current-format documentation only when the new schema and corresponding readers/writers are qualified; retain historical evidence.

### Revision 4 — dependency-ordered engineering gates
The detailed S0–S7 sequence in ON_DISK_FORMAT.md section 20.18 supersedes any implication that the earlier checklist items can be implemented in arbitrary order. Start with isolated wire schema and commit/arena modelling, then ownership/location/trees, portable single-device parity, recovery/Unformatter, native integration, optional protection profiles and final evidence. Every gate remains open. Keep existing 0.19 release and qualification intact. No formatter or media version change until corresponding contracts are frozen and qualified.

### S0 implementation progress
- [ ] Experimental envelope and probe record framing implemented in an isolated C conformance target; execution evidence pending. No production schema or feature ID registered.
- [ ] Complete typed tree, root/commit, geometry, arena and ownership encodings and freeze after modelling/qualification. S0 remains open.

### S0/S1 executable-model progress
- [ ] Bounded immutable record tree, three-lane commit publication, cold selection and verified copy healing implemented in isolated test target; exact-source results pending.
- [ ] General typed scalable tree, allocator/arena and durable ownership/reclamation remain open. The fixed test banks are not a production storage design.

### S2 lifetime model progress
- [ ] Bounded persistent node/edge accounting, retained-root transitions and deferred reclaim model implemented through the S1 committed tree; exact-source execution pending. Production allocation/location engine and real native unformat remain open.

### S2/S3 physical-payload integration progress
- [ ] Bounded single-device file/physical payload, shared integrity, snapshot/reflink, relocation and recoverable root-switch model implemented; exact-source execution pending. Production paged allocation/reuse, full namespace and native integration remain open.
- [ ] Raw 512-byte-aligned original-region discovery and read-only reconstruction for the experimental profile implemented; exact-source execution pending. Production scan batching, geometry profiles and actual foreign-format evidence remain open.
- [ ] Actual foreign-formatter regular-file characterization harness implemented; ext4 runs when installed, FAT32/exFAT/NTFS profiles are explicit. Record exact-source results and expand to real devices, first mounts and Windows-native formatting before selecting production placement.

### Verified experimental milestone (25 November 2026)
- [x] Execute the bounded schema, store, ownership, physical-payload/native-undo and raw-discovery conformance targets together on Linux (exact source `d67cdb9c`, see QUALIFICATION.md).
- [x] Run one real ext4 nodiscard format experiment on an isolated 64 MiB image; verify surviving proof and all three test names' payloads.
- [ ] Complete the scalable CoW tree/arena, allocation reuse with automatic fallback retirement, multi-extent namespace and production byte schema. Integrate the full portable engine, native adapters and production Unformatter; qualify encryption, replication, geometry, real formatter matrix and physical media. Experimental checkmarks do not close S0–S7 or authorize format 0.20 publication.

### Multi-unit allocation integration
- [ ] Bounded multi-block payloads, indexed per-unit integrity, shared relocation and read-only partial recovery visitor implemented; exact-source test results pending. General extent trees, fragmented allocations, scalable ownership/reuse and full portable namespace remain open.

### Variable-height tree candidate
- [x] Linux exact-source multi-unit test at `a2e607224e6cd3fac7f918c09edc95b6dae4d237`: all six experimental tests passed, including 96 write/relocation interruption cases and partial recovery.
- [ ] Variable-height path-copy tree module and conformance test implemented; execution pending. Integrate with scalable arena/ownership and commit publication; qualify rebalancing and page retirement before replacing the bounded store.

### Committed paged-tree integration
- [ ] Variable-height tree connected to the common commit protocol and private append arena, with offline proof discovery and injected-failure conformance; exact-source results pending. Migrate typed ownership/namespace records and implement durable arena reclamation before production integration.

### Atomic multi-record integration
- [x] Variable-height tree executed at `479fb2668e6474df26fd93f9d77a53ebad1b6354`: 96 keys, six levels, 18 injected private-write failures; all seven experimental tests passed.
- [ ] Ordered private-root batch and payload dependency hook implemented; exact-source execution pending. Migrate typed ownership and volume records to this store.

### Typed paged-volume integration
- [ ] One shared semantic volume/ownership engine integrated with paged storage; exact-source execution pending. Replace whole-model traversal/diff and bounded contiguous allocations with targeted typed updates, scalable allocation/reclamation and complete namespace before production integration.

### Experimental image export
- [ ] Read-only named file/verified fragment exporter and regression test implemented; exact-source execution pending. Production Unformatter, complete namespace and in-place restoration remain open.

### Verified integrated experimental milestone (25 November 2026)
- [x] Path-copy trees, paged commit publication, atomic typed-record/payload batches, and shared typed paged-volume ownership model exercised together (147 typed volume interruption cases).
- [x] Non-installed read-only image exporter exercised with complete original names and partial verified fragments; eleven dedicated tests pass at `ce9dce04e9d3dabcd9243f5198d0f103c4858441` (see QUALIFICATION.md).
- [ ] Replace bounded append-only arenas with scalable persistent allocation, reclaim and reserve management; complete multi-extent namespace, full portable semantics and native readers/writers.
- [ ] Finish production Unformatter, freeze recovery geometry using actual formatter/device evidence, qualify security/protection profiles and Common V2 BOOT. The existing production 0.19 format remains active.

### Payload retirement/reuse candidate
- [ ] Explicit unlink, retention release, transactional physical quarantine and reuse after fully protected fallback-bank retirement implemented; exact-source execution pending. Scalable range indexes, persistent metadata-arena recycling and full portable/native integration remain open.

### Metadata arena retirement/reuse candidate
- [ ] Explicit verified tree reachability collection, unchanged-root successor publication and committed free-run consumption implemented; exact-source execution pending. Dynamic paged arena accounting, bounded incremental GC and scalable allocation remain open.

### Verified bounded physical reuse milestone (25 November 2026)
- [x] Payload and metadata reuse in the same committed storage engine: 13 dedicated tests passed at exact source `8f16a57ca02ad362443e040c70e6fdf120362fd4`, including 195 payload-reuse and 87 arena-collection/reuse interruptions plus 32 two-slot reuse cycles.
- [x] Expanded paged ext4/FAT32/exFAT/NTFS characterization and complete-copy arena retirement check passed at exact source `c4bc7dba6f40362a81f6a92673448943cd5e0d6d` (17 dedicated tests; see QUALIFICATION.md). exFAT destroyed all three sample named payloads while retaining discoverable metadata; correct refusal is a characterization pass, not a recovery success.
- [ ] Production scalable allocation, full namespace, native integration, production placement and physical qualification remain open. The exFAT result must inform placement/protection work; it does not authorize production format activation.
