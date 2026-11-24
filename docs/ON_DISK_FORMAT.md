<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS On-Disk Format 0.19

This document explains the persistent Format 0.19 model. Exact packed field order, sizes, constants and feature identifiers in `include/infilfs/format.h` are the byte-level authority. Architectural intent lives in `ARCHITECTURE.md`; feature completion and qualification do not belong in this specification.

Pre-1.0 development accepts the current development format only and does not promise readers or migrations for superseded development formats.

The proposed successor is specified below under [Proposed InfiltratorFS On-Disk Format 0.20](#proposed-infiltratorfs-on-disk-format-020). It is not the current implementation contract.

## 1. Encoding and block model

- Filesystem block size is 4096 bytes (`block_shift = 12`).
- Persistent integers are little-endian.
- Persistent records use explicitly packed layouts; compiler-native ABI layout is never authoritative.
- Current reserved fields, unused checksum bytes and record padding are canonical zero unless a feature/version contract defines otherwise.
- CRC-valid data that violates current structural, reserved-field or feature rules is still invalid.

Persistent metadata objects occupy one filesystem block. Tree pages and paged extent metadata also occupy checksummed filesystem blocks. File data is addressed in logical 4096-byte units even when its physical representation is sparse, shared or compressed.

## 2. Volume geometry and checkpoints

A newly formatted volume of `N` committed filesystem blocks initially places three checkpoint copies at:

- block 0;
- `floor(N/2)`; and
- `N-1`.

Those positions are recorded in each checkpoint. The committed filesystem size may be smaller than the physical backing device. Grow/shrink relocates secondary checkpoints near the new midpoint and end while preserving block zero. Readers never infer checkpoint positions from the backing device's current midpoint/end.

Two permanent recovery locators occupy blocks **1** and **2047**. Both are reserved in every allocation graph, including retained generations, and may not hold allocation pages, objects, tree pages or file data. The minimum volume is 4096 blocks (16 MiB); backing size must be a multiple of 4096 bytes. The formatter places bootstrap allocation metadata around these reserved positions.

Each locator uses the canonical encoding in `include/infilfs/locator.h`: magic `INFSL019`, schema 1, an 80-byte header, Format 0.19 identity, hinted generation, committed block count, three checkpoint offsets, filesystem UUID and CRC64-ECMA at byte 72. All remaining bytes are zero. Each locator is checked independently against physical capacity. Its generation is a hint and may lag ordinary commits.

Discovery examines the two fixed locator maps and a valid block-zero map independently, retaining at most nine map/checkpoint candidates. Valid maps must agree on filesystem identity. A stale locator must not hide newer primary geometry. Every candidate must agree with its source map and pass checkpoint and committed-graph validation before recovery or repair. If all three bootstrap records are lost, ordinary mount fails without scanning the device.

Ordinary publication flushes CoW dependencies, then writes and flushes each checkpoint separately, rotating the first copy by generation. Locator blocks remain untouched. For relocation, dependencies become durable before locator A is written and flushed; the new checkpoints are then individually written and flushed in the order secondary 1, secondary 2, primary 0; locator B is written and flushed last. Retaining the old primary until new backups are durable preserves independent old-geometry discovery even if locator B was already damaged. The first durable checkpoint commits the generation. Resize reports success only after locator B is durable, permitting physical reduction after shrink. Publication errors prohibit further mutation until close and recovery.

Writable recovery preserves matching checkpoint survivors, repairs other copies one at a time with individual flushes, then repairs stale or damaged locators one at a time. Read-only recovery never repairs media. The ordering implementation is shared by portable and native writers in `include/infilfs/checkpoint_protocol.h`.

A checkpoint records the generation, committed total/free block accounting, allocation-tree root, object-index root, namespace root, checkpoint locations, filesystem UUID, root object identity, feature sets, label and integrity metadata.

Filesystem UUID, root object identity and generation are nonzero.

Each physical checkpoint copy is independently validated for magic, exact Format 0.19 identity, header/block geometry, feature compatibility, canonical reserved bytes, checksum and recorded checkpoint-position consistency before its referenced graph can be considered.

Recovery considers candidate checkpoints in descending generation order and selects the newest complete structurally valid committed graph. Structural corruption may justify fallback to an older committed generation. External I/O failures, unsupported features, memory failure or other operational errors must not be silently reclassified as corruption merely to force fallback.

## 3. Feature compatibility

Format 0.19 uses separate compatible, read-only-compatible and incompatible feature classes.

- Unknown incompatible bits prevent open.
- Unknown read-only-compatible bits prevent writable open.
- Unknown compatible bits may be ignored according to their contract.

Current Format 0.19 requires UTF-8 names, sparse extents, the allocation-tree representation and Unicode-name policy v1. Unicode-name policy v1 validates UTF-8 but performs no implicit Unicode normalization: namespace components are preserved exactly as supplied. Unflagged volumes compare those bytes exactly. The optional `INFS_INCOMPAT_CASEFOLD_V1` policy changes lookup identity by folding ASCII `A` through `Z` to `a` through `z`; every other UTF-8 byte remains identity-significant. Original spelling is retained in directory entries. The fold is deliberately versioned and independent of host Unicode tables, so any broader Unicode case-fold or NFC/NFD policy requires a new feature/version identity rather than silently changing lookup semantics. Current formatters enable the established known feature set for inline data, shared extents, paged metadata/extents, symbolic links, hard links, snapshots, object-index tree, directory tree, allocation tree and compressed extents.

A representation is accepted only when its feature bit/version contract agrees with the structure actually stored. A feature bit that contradicts the selected record version is corruption.


### Optional case-fold policy v1

`INFS_INCOMPAT_CASEFOLD_V1` is an optional volume-wide namespace policy.
Directory lookup, duplicate detection, scalable directory-tree routing and
native adapter dentry hashing all use the same canonical comparison: ASCII
uppercase letters fold to lowercase and all other valid UTF-8 bytes compare
exactly. A case-only spelling difference therefore cannot create a second name,
while non-ASCII code points remain distinct unless their UTF-8 bytes are
identical. The stored directory entry retains the spelling supplied at creation.

This deliberately narrow first policy is stable without depending on an
operating system's evolving Unicode tables. Full Unicode case folding, if
introduced later, is a different incompatible policy and cannot redefine v1.

### Removable-volume filename profile v1

The optional incompatibility feature `INFS_INCOMPAT_REMOVABLE_NAMES_V1`
selects a conservative cross-platform namespace profile for volumes intended
to move between operating systems. It does not change Unicode identity:
component bytes remain valid UTF-8 and are compared exactly under Unicode
policy v1.

When the feature is present, each namespace component is limited to 255 UTF-8
bytes, must not end in a dot or space, must not contain ASCII control bytes or
`< > : " / \\ | ? *`, and must not use the case-insensitive DOS device base
names `CON`, `PRN`, `AUX`, `NUL`, `COM1` through `COM9`, or `LPT1`
through `LPT9`, including those bases followed by an extension. Readers must
reject a flagged namespace that violates this contract. The profile is
optional; unflagged volumes retain the general 1023-byte UTF-8 component
contract.

## 4. Allocation ownership

Logical allocation ownership remains one authoritative bit per filesystem block: zero means free; one means allocated or unavailable.

Format 0.19 persists that bitset as a checkpoint-rooted copy-on-write allocation tree rather than a monolithic bitmap image.

Allocation leaves use `INFSAL01`; branch pages use `INFSAB01`. Allocation pages carry generation, logical index, level, entry count, payload length and CRC64 metadata.

The current packed allocation-page header is 72 bytes, leaving 4024 payload bytes in one 4096-byte page. Each leaf therefore represents 32,192 allocation bits except the final partially used leaf. Branch payloads contain little-endian 64-bit child block pointers with fanout 503. Format 0.19 fixes the allocation root at level 3.

The checkpoint stores the allocation root and leaf count. The declared leaf count must agree with committed `total_blocks`. Missing, duplicate, out-of-range, malformed or checkpoint-overlapping allocation pages are corruption.

Allocation pages are bootstrap metadata, not persistent namespace objects, and have no object IDs. Unchanged pages may be shared by later committed generations, so a referenced page generation may be older than the selected checkpoint but never zero or newer than that checkpoint.

Transactions maintain the authoritative allocation state plus rebuildable runtime free-extent acceleration. Publication rewrites only affected allocation leaves and the required replacement branch paths/root. Whole-volume bitmap cloning or whole-bitmap publication is not part of Format 0.19.

Every allocated block must be accounted for by the committed graph: checkpoints, allocation pages, metadata tree pages, indexed objects, extent metadata, file data or retained snapshot state. Metadata ownership overlap is corruption. Shared ordinary file-data ownership is permitted only by the shared-extents contract.

## 5. Persistent metadata objects

Each metadata object begins with `struct infs_object_header_disk`, containing:

- object magic;
- object type and version;
- generation;
- 128-bit object ID;
- parent/recovery relationship ID;
- payload size;
- checksum algorithm; and
- reserved checksum field.

Object IDs and generations are nonzero. Payloads, padding and reserved bytes must be canonical for the selected object type/version.

Object types include directory, regular file, object index, checksum metadata,
symbolic link, snapshot catalog, portable principal and portable security
descriptor.

### Portable principals and security descriptors

When the portable-security incompatibility feature is set, portable principals
and security descriptors are independent indexed object classes. Current
security payload version 2 is the sole accepted development representation;
superseded payload versions are corruption.

A principal object's 128-bit object ID is the stable principal ID. Its payload
contains principal kind plus zero or more fixed-size typed binding records with
an explicit meaningful-byte count. POSIX UID/GID values are encoded as a
nonzero 128-bit identity-authority ID followed by a little-endian 32-bit number.
Windows identity uses canonical binary SID bytes. Opaque bindings are
preservation-only and are not eligible for reverse credential lookup.

A reserved implicit principal-ID namespace defines OWNER, GROUP, EVERYONE,
CREATOR_OWNER and CREATOR_GROUP. Reserved IDs are never allocated to ordinary
objects or stored as ordinary principal objects.

Resolvable principal bindings are accompanied by
`INFS_OBJECT_SECURITY_BINDING` secondary-index objects. Their object IDs are
bounded collision-slot candidates derived from a domain-separated SHA-256 of
the complete canonical binding. The record stores the complete binding, full
digest, slot and target principal ID, so the ordinary object index provides
bounded reverse lookup without making a hash prefix authoritative. Opaque
bindings are never indexed.

A namespace object's `security_object_id` references a shareable security
descriptor. The descriptor contains owner and primary-group principal IDs,
descriptor-control flags, a canonical semantic digest, a collision slot and
ordered ACEs that reference principal IDs. Its object ID is deterministically
derived from the full descriptor digest plus the slot. Descriptor object parent
ID is zero because one descriptor may be referenced by many namespace objects.

Writers probe a bounded set of content-derived descriptor IDs and reuse an
exact descriptor when one already exists; ordinary ACL mutation never scans the
object population for deduplication. Descriptor reachability is derived from
the namespace graph, not a mutable persistent reference counter. Foreground
namespace mutation never performs whole-namespace reference scans for
descriptor reclamation. Unreferenced descriptors/principals are tracing-GC
candidates during scrub or a bounded maintenance pass.

Compact descriptors keep ACEs inline. Descriptors larger than the inline
capacity use checksummed metadata pages under the same descriptor object ID.
The public ACL model is therefore not bounded by one filesystem block.

The exact persistent and evaluation contract is in
`PORTABLE_SECURITY_OBJECTS.md`.

## 6. Object index

The object index maps a persistent 128-bit object ID to the physical block containing that object's metadata plus its type.

Current Format 0.19 uses a generation-aware radix/tree representation. Directory entries refer to persistent object IDs rather than physical object blocks, so metadata relocation changes the index rather than every namespace reference.

Index tree pages are independently bounds/allocation/owner/generation/checksum validated. Entries require nonzero unique object IDs, valid target blocks, matching object identity/type and canonical flags.

The root object must appear exactly once and must agree with the checkpoint's recorded root identity and root block relationship.

## 7. Common attributes

Portable common attributes contain:

- logical size;
- link count;
- a 64-bit portable flags/storage-policy word;
- birth/access/modification/metadata-change timestamps;
- portable security-object ID; and
- portable extended-metadata object ID.

These are not Linux `struct stat` or Windows file-information structures.

The low portable-flag bits represent presentation semantics such as read-only,
hidden, system, archive, temporary and not-content-indexed. The high policy bits
are disjoint persistent storage policy:

- bits 48..55: requested protection-copy count;
- bits 56..63: encryption-domain ID.

A zero protection-copy count means the backend default (all members for the
current synchronous mirror). A nonzero count selects that many deterministic
replica members from persistent object identity. Encryption domain zero uses the
encrypted container's root volume key; nonzero IDs derive domain-separated
AEAD subkeys and are authenticated as part of encrypted block AAD.

Storage policy applies to object data, not structural metadata/checkpoints.
Nondefault-policy regular-file data is therefore extent-backed even when small
enough for the ordinary inline representation. Metadata-only policy changes on
a nonempty file are invalid because they would reinterpret existing extents
under a different placement/key without relocation.

The security reference may be nonzero when the portable-security incompatibility
feature is enabled. The extended-metadata reference may identify the current
typed-extension or metadata-set/named-stream object family when its feature
contract is enabled.

A POSIX compatibility record stores current mode/UID/GID information for adapter use. It is compatibility metadata, not persistent object identity or the final portable security authority.

## 8. Directories and names

Current directory heads reference checksummed directory-tree pages. Hashing selects a storage path using the volume's declared namespace policy. The default identity and comparison are byte-exact and case-sensitive; volumes declaring `INFS_INCOMPAT_CASEFOLD_V1` instead fold ASCII A-Z for lookup and tree routing while retaining the exact original UTF-8 spelling in each directory entry.

Stored component names:

- are valid UTF-8;
- contain 1 to 1023 bytes;
- contain neither NUL nor `/`; and
- never store `.` or `..`, which remain traversal syntax.

Variable-length directory records are aligned and include record length, name length, target type, flags, 128-bit target object ID and exact name bytes. Current unused flags/padding are zero.

Directories and symbolic links are single-parent objects. Regular files may have multiple incoming directory references when hard links are enabled. A regular file's stored link count must equal its reachable directory-reference count. Hard links to directories or symbolic links are unsupported.

## 9. Symbolic links

A symbolic-link object stores common/POSIX attributes followed by target length and target bytes.

The target is nonempty UTF-8, contains no NUL and is bounded by the current one-block symbolic-link payload capacity. It may be absolute or relative. The portable core stores the target but operating-system adapters own path-resolution semantics.

## 10. Regular files

A regular file may be:

- empty;
- inline;
- ordinary extent-backed;
- sparse through hole extents;
- shared through reflink/shared normal extents; or
- compressed through bounded compressed normal extents.

### Inline data

Small non-empty files with default storage policy may store their payload and logical SHA-256 digest in the file metadata object and own no separate data blocks.

Current `INFS_INLINE_DATA_MAX` is 3840 bytes. Growing beyond the inline bound promotes the file transactionally to extent-backed representation. Shrinking may return to inline representation only while storage policy is zero. A file with nonzero protection-copy or encryption-domain policy remains extent-backed so every data byte passes through policy-aware storage I/O.

### Ordinary and sparse extents

Extent-backed files provide complete logical coverage through data and hole extents. Hole ranges read as zeros and own no physical data blocks.

Fragmented files may move extent descriptors into checksummed paged extent metadata. Those pages are part of the ownership/integrity graph.

### Shared extents

Shared normal extents implement reflinks. Writes break sharing through copy-on-write. Physical data may have multiple logical owners only under the shared-extents feature; metadata blocks may not be multiply owned where a tree requires unique ownership.

### Compressed extents

Compressed extents record codec identity, stored-byte length and logical extent length. Current automatic writes use IAC1 v1 (codec ID 2); LZ4 (codec ID 1) remains a supported non-default representation.

Compression is selected only when it saves filesystem blocks. Logical SHA-256 covers the uncompressed logical bytes. Operations that would slice a compressed stream must materialize or replace the affected bounded cluster rather than pretending compressed logical blocks map one-for-one to physical blocks.

The detailed codec contract is in `COMPRESSION.md`.

## 11. File-data checksums

Logical file data is SHA-256 protected. Checksum metadata belongs to the same committed graph and is validated together with file representation.

Sparse holes contribute logical zero bytes to the checksum model without owning physical data. Inline data uses the same logical-block checksum meaning even though bytes live inside the metadata object.

Scrub must reject missing, duplicate, mismatched or structurally invalid checksum coverage.

## 12. Snapshots and retained generations

Named snapshots reference immutable earlier generation roots and their retained allocation state.

Blocks remain unavailable for reuse while referenced by the live graph or any retained generation. Deleting a snapshot may reclaim only blocks that are unreachable from every remaining live/retained graph.

Snapshot reads are read-only. Live writes must copy on write rather than mutate blocks still referenced by a snapshot.

Format 0.19 does not require snapshot-aware resize migration. An implementation may conservatively reject resize while retained snapshots exist.

## 13. Transaction publication

Critical metadata mutation is copy-on-write.

A successful transaction publishes a new generation only after required replacement data/metadata/allocation structures are written and required durability operations complete. The first durably published new checkpoint is the commit point. Later checkpoint replicas may be refreshed after that point.

A crash before commit leaves the prior committed generation authoritative. If durability of the first checkpoint publication cannot be established, writable operation must fail closed until reopen/recovery.

Operation-level savepoints may roll back the current operation's tentative changes while preserving earlier acknowledged buffered changes.

## 14. Online geometry change

Committed filesystem geometry is independent of physical backing capacity.

Grow/shrink uses the ordinary transaction/checkpoint model to publish replacement allocation geometry and, when necessary, relocated checkpoint positions.

Shrink must fail before commit if any live allocation would fall outside the requested new geometry. Failure must preserve the previously committed geometry and data.

## 15. Integrity and graph validity

A structurally valid Format 0.19 graph requires:

- valid checksums and canonical encodings;
- in-range allocated physical references;
- exact object identities/types at referenced blocks;
- valid parent/reference/link-count relationships;
- no forbidden metadata cycles or multiply aliased tree pages;
- complete ownership accounting;
- exact namespace reachability; and
- valid retained snapshot ownership.

Unknown or malformed metadata must not be accepted merely because its CRC happens to validate.

`fsck.infiltratorfs --scrub` validates the authoritative reachable graph. `infilfs-forensic` may discover recognizable authenticated metadata physically, but forensic discovery alone is not committed state.

## 16. Adapter metadata

Platform-specific metadata must remain separate from the portable format concepts it does not define.

Current Linux metadata can preserve standard xattr namespaces and special-node information without turning Linux UID/GID/xattrs into the universal security model. Future portable security and named-metadata object classes will require explicit feature/version contracts.

See `PLATFORM_ADAPTERS.md` and `SECURITY.md`.

## 17. Normative sources

For Format 0.19 maintenance, use these sources in order:

1. `include/infilfs/format.h` for exact persistent constants, packed layouts and compile-time size contracts.
2. This document for the persistent structural model and acceptance intent.
3. `ARCHITECTURE.md` for non-byte-level design invariants.
4. The format/core test suite for executable regression requirements.
5. `QUALIFICATION.md` only for exact-source evidence; qualification history does not redefine the byte format.

No second conformance document is maintained. This avoids duplicating format rules and allowing two specifications to drift.


### Generic typed extension objects

`INFS_INCOMPAT_TYPED_EXTENSIONS` enables immutable
`INFS_OBJECT_EXTENSION` objects. The namespace object's existing
`extended_attributes_object_id` references one extension envelope containing a
stable 128-bit type ID, type-specific version, generic flags, byte length,
SHA-256 payload digest and opaque bytes.

`INFS_EXTENSION_FLAG_REPARSE` identifies reparse/path-redirection semantics
without importing Windows reparse tags into the portable core.
`INFS_EXTENSION_FLAG_PRESERVE_OPAQUE` requires adapters that do not understand
a type to round-trip it unchanged. Extension objects have zero parent IDs and
immutable contents. Snapshot and reflink operations retain the reference;
replace/detach mutates only the namespace object's reference. Unreachable
extension objects are tracing-maintenance candidates rather than requiring an
O(N) foreground reference scan. Named streams remain a separate roadmap item.

---

# Proposed InfiltratorFS On-Disk Format 0.20

**Status: proposed architectural specification, revision 1 (25 November 2026).**
This section defines the proposed successor to Format 0.19, not the format currently written by the software. The existing Format 0.19 specification above remains authoritative for that implementation. No format identifier, magic, feature bit, binary offset, release number or implementation change is authorised by this text alone. When the proposal is accepted and its byte-level schema frozen, the canonical definitions and all writers/readers must change together. Software release and on-disk version remain independent. No backward mount or migration obligation is imposed during pre-1.0 development; older media must be recognised and refused, never implicitly reformatted.

## 20.1 Purpose and design principles

The objective is a coherent production-quality filesystem, not compatibility with another filesystem's internal representation and not a patch set around Format 0.19. The architecture must make native quick-format undo a deterministic operation under a stated preservation contract, while offering independently verifiable best-effort recovery from foreign quick format and repartitioning. It must also remove avoidable structural limits, scale to large storage, and retain the existing transaction and portable namespace guarantees.

Preserve the 128-bit object identity model, portable little-endian encoding, 4096-byte filesystem blocks, generation-based CoW, transactional directories, extent-backed sparse/shared/compressed data, native IAC1 compression, SHA-256 logical integrity, portable principals/ACLs, named streams, nanosecond timestamps, quotas, and policy-aware storage. These are design choices, not constraints against replacing a demonstrably inferior representation. The single-device implementation must be complete without encryption, mirroring or a particular operating system.

Prefer one authoritative model for each concept, with reconstructible accelerators rather than competing sources of truth. A persistent structure earns its space by providing a correctness, recovery or measured scalability benefit. No silent degradation of integrity, durability, protection, security or feature semantics is permitted.

## 20.2 Storage identity, namespaces and geometry

Separate the identity of the physical allocation/recovery domain (storage-set UUID) from each logical filesystem incarnation (volume UUID). Both are independently generated nonzero 128-bit values. A member UUID identifies an individual backing member and is never inferred from enumeration order. A single-device set is the one-member case. The volume UUID changes on recoverable format; the storage-set UUID does not. Object identity is scoped by volume UUID. Reinstating an archived volume reinstates its original UUID and object identities; the globally monotonic storage-set transaction sequence does not roll back.

Persist the committed logical block count, region layout and member geometry independently of current backing capacity. All filesystem addresses are relative to the original filesystem start, not the enclosing disk or present partition table. Discovery records additionally identify their own expected relative block address and original geometry; a raw disk scanner may derive candidate start offsets from the observed physical record address, but must corroborate geometry across independent records and reject overflow, overlap, or impossible bounds. A partition move is not a change of logical block identity. No host-specific device path, drive letter, or partition number is persistent authority.

Use checked unsigned 64-bit block/range arithmetic with 128-bit intermediates or equivalent checked operations; the actual maximum supported size is the minimum of format addressability, tree depth, byte-address arithmetic, backend and platform limits. No claim of supporting every theoretically representable block count is made. Retain 16 MiB as a candidate minimum only if all mandatory redundancy and transactional reserve fit; otherwise publish a larger minimum, never silently omit recovery protection.

## 20.3 Canonical records and persistent roots

All on-disk metadata is self-identifying and versioned. The common record envelope shall contain a format-family marker, record kind and version, header/payload lengths, storage-set UUID, stable logical record/page ID, owning volume UUID (or explicit set-global scope), creating transaction sequence, logical role/range, copy ordinal, expected relative address, and CRC64-ECMA. Reserved fields and padding are zero. A canonical SHA-256 content digest covers the logical record identity, type, version, owner, generation and payload; physical copy ordinal/address and physical CRC are excluded from this digest so replicas have identical logical identity. Physical CRC covers the complete encoded block with its own field zeroed. Define exact domain separators, encoding, algorithm identifiers and field widths in the byte schema. Reject unknown required versions, invalid canonical encodings and inconsistent reference types before following pointers.

The authoritative set checkpoint contains the storage-set UUID, transaction sequence, committed geometry, active-volume descriptor reference, retained-volume catalogue root, allocation root, extent ownership root, member/protection root where enabled, recovery manifest reference, feature/algorithm profile and previous committed checkpoint identity. References to critical metadata carry expected logical identity and digest plus physical replica locations, with no requirement for a potentially lost object index to locate those replicas. The previous checkpoint is a recovery relation, not an unlimited retained-history promise.

Each volume descriptor contains its UUID, label, namespace root identity, object index, security and quota roots, snapshot catalogue, policy profile and relevant historical geometry. A named snapshot is an immutable volume-state descriptor, not a second copy of the whole storage set. Avoid cycles: set checkpoint -> volume/snapshot catalogue -> volume-state roots; a snapshot state must not recursively capture the mutable snapshot catalogue or the global allocator. Retained-volume records preserve the catalogue as it existed on retirement.

The byte schema must establish fixed-size bootstrap envelopes and bounded tree depth, variable-length typed leaf records, canonical ordering, unique keys, checked occupancy and explicit empty-tree representation. Persistent pages are 4096 bytes; larger objects use trees of pages rather than larger indivisible records. The exact packed layouts are a subsequent schema-freeze deliverable, not invented constants in this proposal.

## 20.4 Regional placement and independent recovery

Divide each storage domain into deterministic allocation/recovery regions with a versioned size/profile. Candidate default: 1 GiB regions, with a tail region and a small-volume profile; assess overhead and device geometry before freezing it. Reserve explicit recovery space near the beginning, end and in separated interior regions. Ordinary fast mount probes a fixed bounded set of bootstrap anchors derived from the declared profile and known backing bounds; when physical geometry changed, surviving anchors provide old committed geometry. A full scan is never a mount fallback.

Use at least three physically separated checkpoint publication locations for an ordinary full-sized single-member volume, with explicit disjoint placement rules and enough space for old and replacement committed records. Store at least two independently located copies of every mandatory structural metadata page (including allocation, ownership, index, directory, extent, checksum, security, snapshot and recovery catalogue pages), with copy separation across recovery regions whenever supported. The small-volume profile must still provide two nonoverlapping placement groups. File payloads retain their explicit protection policy; metadata redundancy does not imply two copies of all file data. Inline payloads inherit metadata redundancy. On one disk, regions are overwrite-risk separation, not independent hardware fault domains.

Use independent recovery manifests referencing the same canonical committed metadata rather than parallel mutable namespace copies. A manifest records set/volume identity, transaction identity, geometry, root references and digests, retained generation descriptors, and enough self-identifying metadata-location information to locate and validate replicas even when normal bootstrap is damaged. Maintain recoverable manifest copies and bounded normal mount anchors; offline scans locate manifest/checkpoint records and metadata pages at any surviving original geometry. A secondary metadata page shall carry logical role, owner and covered key/range, allowing offline reconstruction of surviving trees when parents are lost. Historical objects are candidates, not automatically authoritative; an immutable valid record need not have belonged to a committed transaction.

Placement must be evaluated against actual write/discard ranges of foreign quick formatters (including ext4, NTFS, exFAT, FAT, and other representative storage formats) and partition editors. Do not choose fixed offsets solely because they look distant. Recovery-critical copies must not share the same predictable overwrite region when the geometry permits. Record evidence and residual failure domains. An anchor may locate a commit but never substitute for it; no checksum, timestamp or highest sequence alone grants authority.

## 20.5 One allocation and ownership model

Retain the useful sharded hierarchical free-space structure but make ownership explicit and persistent. Proposed primary model: an extent ownership tree keyed by physical start and length, storing allocation identity, kind, protection/encryption domain, birth transaction, and checked references/backreferences. The authoritative set-wide allocation state describes free, owned, reserved and quarantined/unavailable ranges; the region bitmap/free-extent summaries are derived indexes committed atomically with ownership, not independently editable truth. Use a sparse range representation for large free regions and paged bitmap leaves where fragmentation makes them efficient. Select a canonical encoding and cross-validation rule so that identical extents cannot be simultaneously free and owned.

Represent shared file allocations once and reference them from immutable extent descriptors; track reverse references or equivalent checked incoming edge counts for shared tree pages and data. Snapshot creation and volume retirement add a root reference rather than enumerate every reachable extent. Backreferences must identify immediate owning page/object edges (not every transitive snapshot/file path). A root reference retains the entire immutable subtree. Reference removal cascades through a durable, bounded reclamation queue; zero-count pending records remain unavailable until reclamation commits. No allocation with an incoming reference, unresolved transaction, active recovery pin or required fallback root may become free, be discarded or be relocated in a way that invalidates its owners. Rebuild/repair ownership only from verified roots and edges, with disagreements quarantined, not guessed free.

Use reserved bootstrap/reclamation space for allocation-tree self-hosting and transaction publication. Reserve pessimistic space before mutation; normal allocations cannot consume the emergency reserve. Define bounded transaction/reclamation batches, reserve replenishment, ENOSPC rollback and near-full behavior without recursive unbounded allocation. Unreachable prepared records are reclaimed only after committed authority is established. Quotas distinguish logical charged bytes, physical shared storage, retained-volume usage and metadata/reserves; quota-policy changes cannot silently expose retained data to reuse.

## 20.6 Transaction, commit and failure protocol

A single transaction atomically changes active volume, retained catalogues, ownership and all affected metadata. Sequence numbers are monotonic within the storage set; overflow refuses mutation. Metadata and shared/retained payloads are immutable after publication. Adaptive NoCoW is permitted only for explicitly eligible uniquely owned file data when its documented atomicity/checksum contract is met. An overwrite that would invalidate a retained root or the guaranteed crash fallback is forbidden; use CoW or a fully durable undo/redo protocol instead. NoCoW must not weaken the native unformat guarantee.

Publication order:
1. Acquire the writer authority and reserve the complete required metadata/data space, without invalidating existing committed roots.
2. Write replacement payloads, both mandatory metadata copies, ownership state and recovery-manifest copies; validate references and flush every required member.
3. Write a new checksummed (authenticated when the storage profile provides authentication) commit record identifying the complete candidate root set. The first durably published valid commit record is the single-device commit point. Its sequence and root digest bind all dependent state. On a mirrored set the separately defined member acknowledgement/fencing rule must also be satisfied before durability is acknowledged.
4. Replicate the commit at separated locations, writing/flushing each independently, without destroying the last independently discoverable previously committed state before the replacement can be established. Update geometry discovery anchors using the same old-or-new ordering.
5. Return durable administrative success only after all required publication copies and discovery records are durable. Ordinary buffered writes retain the native API's separate acknowledgement/fsync semantics.

On ambiguous I/O or failed flush, fence further writes and reopen to establish committed authority. A reported failure does not imply the old transaction necessarily won; expose that distinction to the caller. Never fall back merely because a newer supported graph could not be read due to operational failure, resource exhaustion or unsupported features. Read-only inspection performs zero repair writes. Writable healing validates authority, repairs replicas one at a time, then resumes mutation. If a recoverable previous checkpoint is retained for fallback, its reachable blocks remain pinned until that guarantee expires in a subsequent committed transaction; do not advertise a fallback whose dependencies have already been reused.

## 20.7 Native recoverable quick format and deterministic unformat

Define recoverable quick format as an explicit privileged set transaction, not a fresh destructive formatter. On a valid Format 0.20 storage set it creates a new empty logical volume with a new UUID, while preserving the old volume descriptor, its named snapshots, security, quotas, policies, keys and complete reachable ownership as a retained-volume catalogue entry. The set allocator and sequence remain authoritative. No initialisation, discard, zeroing, key rotation or repartitioning may destroy the old graph. Metadata creation and namespace replacement are CoW. The old volume remains allocated and is not visible as the active namespace. Report retained space, new free space, retention ID, and durable transaction status. Repeated quick formats create separately named/identified retained incarnations; no implicit age/count/space expiry. Refuse rather than silently release history when space is inadequate.

Every formatter entry point (CLI, GUI, native tools, Common V2 and adapters) must share an exclusive target-bound preflight: resolve actual backing identity/extent, verify capacity, mounts/holders and exclusive access, inspect existing format, check required keys and storage-set health, calculate reservations and present the operation class. Revalidate the open handle after user authorisation and before first write. Refused preflight produces zero target writes. Blank-media creation and destructive replacement are distinct operations, requiring explicit destructive intent for unknown/damaged/noncurrent media. Never silently fall back from recoverable format to destructive format. No recovery guarantee is asserted for Format 0.19 media or a non-InfiltratorFS prior filesystem.

Native unformat is the inverse namespace selection transaction: validate the selected retained descriptor, all referenced ownership, graph consistency and required keys; publish it as active under its original UUID at a new set sequence. By default retain the displaced active volume as another historical entry rather than destroy post-format writes. Support read-only inspection and export of any retained volume without activating it. Retention release is an explicit transaction that removes roots and schedules safe reclamation; it is not secure erasure. In-place recovery on a healthy native set needs no physical copying of retained payloads and has an old-or-new durable outcome. Exclusive access, authorization and verified graph are mandatory; for foreign or damaged-media repair require independent verified export/backup first. A failed format or unformat must leave either complete prior authority or complete new authority, never a hybrid, under the stated storage failure model.

## 20.8 Foreign quick-format recovery and the Unformatter

Foreign tools are outside our allocator and can overwrite any bytes. Use a separate read-only raw-device recovery engine (extend the existing forensic scanner or replace its internals where appropriate), independent of ordinary mount. Scan a chosen immutable image/device in bounded memory, testing plausible original alignment/partition starts and region profiles, locating independently valid anchors, manifests, commits and metadata copies. Raw physical scanning of an encrypted container needs the appropriate container reader and authorised keys; ciphertext cannot be interpreted as plain filesystem records. Group by set UUID, volume UUID, transaction identity and original geometry. Reject contradictory layouts, incompatible feature profiles, impossible bounds, cycles, invalid digests, and cross-volume or cross-generation substitutions. A shared older immutable page is valid only where the selected committed reference authenticates its identity and digest.

Select a uniquely supported committed graph when evidence permits. If root copies are missing, reconstruct candidate paths from independently validated object, directory, extent and checksum records; recover only relationships supported by evidence. Orphaned records with no commit proof must be labelled as salvage, not committed files. Resolve hard-link identities once, preserve sparse holes versus missing ranges, decompress only verified bounded encoded clusters, verify logical SHA-256, retain named streams, ACLs, attributes, timestamps, security identities and snapshots where evidence survives. Never treat a missing extent as an ordinary zero-filled sparse hole.

Report per object: verified complete, partial (explicit logical missing/corrupt ranges), ambiguous (competing candidates), or unrecoverable, with separate namespace/metadata and payload confidence. Publish source geometry, original identity, commit evidence, copy provenance, physical read failures, checksum coverage, unsupported features and destination fidelity limitations. Export to separate storage by default, with safe path handling, no automatic execution or following recovered symlinks/reparse points, and a machine-readable manifest. Never modify the source during scan/export. An incomplete or ambiguous graph cannot be mounted writable as restored authority. Foreign in-place restoration requires a verified complete graph, independent verified export/backup, exclusive access, explicit authorisation, transactional publication and post-restore scrub.

A guaranteed native undo is not a guarantee of foreign recovery. Full overwrite, discard/TRIM, secure erase, key loss, controller failure and overwritten unreplicated file data can make recovery impossible. No universal success percentage, time limit or replacement for backup is claimed.

## 20.9 Scalable metadata, checksums and namespace

Use a common immutable paged tree family with explicit typed keys/values for object lookup, directory lookup, extents, logical-block checksums, ownership, snapshots, retired volumes, named streams, typed extensions, quota and security indexes. Choose a balanced CoW B+ tree with ordered leaves as the default candidate where range access is needed; a radix representation remains acceptable for fixed-width identity keys if measurements and simpler validation justify it. Share page/reference validation and split/merge rules where semantically identical; do not force every key type into a single untyped tree. No fixed catalogue pointer array or linked-chain traversal is the required scaling mechanism. Tree height grows within checked format/platform limits; capacities derive from schema and media, not arbitrary one-block catalogue limits.

Index SHA-256 logical-block checksums by object identity and logical block/range. Persist sparse-hole semantics explicitly; inline bytes, partial final blocks and compressed clusters have canonical logical digest rules. Verify compressed representation bounds before decompression, then verify logical data. Retain optional stored-byte checks for early detection, without replacing logical integrity. Metadata checksums detect accidental damage; SHA-256 root binding provides consistency against a selected trusted root, not authentication against a privileged adversary who can rewrite the entire unencrypted graph.

Preserve byte-exact UTF-8 and case-sensitive default, 1023-byte general names and optional removable-name profile unless testing supports a justified change. An optional full Unicode case-fold profile shall pin Unicode data version, exact canonicalisation algorithm and collision rules, retain original spelling, and reject ambiguous conversion; never silently reinterpret an existing namespace. Preserve existing hard-link and symlink semantics, portable principals, ACL mapping and security-descriptor sharing. Allow paged symlink targets, named-stream catalogues and typed extension payloads under bounded resource policy where justified. Unknown opaque extensions are preserved byte-for-byte with their type/version/digest; unknown mandatory semantics prevent unsafe access. IAC1 remains the default codec; format changes to compression require a separately versioned codec and representative benchmarks.

## 20.10 Memory, resize and maintenance

Page allocation and ownership information on demand with bounded configurable caches, regional free-space summaries, dirty-page tracking and transaction reservations. Ordinary mount must not read or allocate a full-volume bitmap; full graph validation/scrub may stream it in bounded memory. Lazy validation must never let unvalidated ownership or untrusted metadata influence allocation or namespace access; validate each dependency before use, with explicit read-only/scrub modes for exhaustive validation. Local updates shall not scan/copy all allocation leaves or all snapshot records. Target complexity: normal keyed lookup logarithmic in tree size, publication proportional to touched paths plus bounded self-hosting closure, snapshot/retirement root operations independent of file count (excluding deferred reclamation, validation and requested export). Bound caches by memory budget rather than device size; OOM must fail closed.

Historical volume/snapshot descriptors record independent geometry. Growth updates set geometry and availability without rewriting immutable historical roots. Shrink refuses any range still required by active, retained, fallback, metadata replica or recovery roots unless a verified transactional relocation updates all affected physical references while preserving logical identity and data. Never invalidate historic geometry silently; physical truncation is allowed only after durable publication and confirmation that no required root references the removed range. Defrag, compact, scrub, repair, trim and GC use the same ownership/placement contract. Common V2 maintenance must not bypass it.

## 20.11 Encryption and storage protection

Version the encrypted backing container independently from filesystem format. Preserve authenticated encryption and wrapped key material; replace the single key-bearing header with at least two authenticated, independently discoverable copies in separated physical placement groups. Headers bind container/set identity, geometry, algorithm/profile, key slots, generation and authenticated key-wrap parameters. Verify bounded KDF parameter policy before expensive derivation; document explicit administrator override where supported. Do not replace a valid last usable header until the new header is durable. Key rotation and retirement preserve keys needed by every retained graph; no plaintext keys on media or in logs.

Use aligned independently authenticated encrypted allocation records (candidate: an explicitly versioned aligned sector/record layout with separate authenticated tag/index pages where justified). Bind ciphertext to container identity, logical address, key domain and record version. Define nonce generation/uniqueness, per-key usage budget and rotation, including crash and concurrent writes; never rely on a resettable filesystem generation alone for nonce uniqueness. Allocation-aware initialisation must use authenticated allocation state to distinguish never-written zeros from missing/tampered records. It cannot simply skip initialisation and accept absent ciphertext as zeros. Retained allocations remain readable; new logical volumes share the backing container without reinitialising it. Detect rollback relative to trusted external state only if such state is available; local authentication alone cannot prevent a complete valid historical replay. Validate any proposed cryptography and failure semantics before freezing encoding.

Mirrored storage persists member/set identities, expected membership, member epoch, committed sequence/digest, placement policy and dirty/resync state. Define deterministic placement independent of enumeration order. Default synchronous protected writes require all designated replicas and durability barriers; a partial/ambiguous write immediately fences further conflicting mutation. A separately specified explicit degraded-write policy may operate only with unambiguous exclusive authority and durable stale-member fencing; without quorum/witness or equivalent external fencing, two isolated members must not independently claim writable authority. Rejoin checks epochs/roots, never treats stale data as equal, and resumes verified range resync from durable progress. Do not infer authority merely from highest generation on an isolated member. Persist protection policy and enforce it on data movement, reflink, snapshots and recovery; metadata copies across a single member are not a substitute for multi-member protection.

Separate recoverable format, retention release, destructive reformat and secure/cryptographic erase. A purge claims only what it can verify: cryptographic erase requires invalidating all usable wrapping/key copies and external recovery keys as applicable; physical erase depends on device/controller capabilities and cannot be guaranteed by ordinary overwrite of SSD logical blocks. Never automatically revive a purged generation. Historical ciphertext, recovery metadata and access policy remain protected.

## 20.12 Canonical implementation and compatibility

One canonical serialized schema and shared validation/publication invariants serve the portable library, native Linux implementation, Windows adapters and freestanding UEFI reader. Generated or explicitly shared C definitions, compile-time layout assertions and cross-platform golden images avoid independent dialects; no compiler padding, native endian or kernel structure becomes disk authority. Keep Common V2 ABI contract distinct from the disk schema and expose capabilities truthfully. BOOT's bounded freestanding read-only profile must parse the same authority, supported feature flags, namespace and integrity semantics through the Common V2 filesystem module; unsupported protection/encryption/codec profiles fail explicitly. No V1 module or FUSE dependency is introduced. Any native writer must obey the same graph and commit rules. No source or feature-completion claims follow from this proposal.

## 20.13 Acceptance and schema freeze

Before implementation, freeze a byte-level schema with exact offsets, magic values, identifiers, digest domains, key layouts, range limits, region placement profiles, commit/anchor slots, reserve formulas and capacity ceilings. Publish state-machine and ownership invariants, deterministic test vectors and a failure model. Resolve open choices (region sizing/small-volume minimum, metadata replica count/overhead, common tree encoding, ownership edge accounting, encryption record/tag layout and member authority protocol) by prototypes and evidence, not expedient compatibility. This section supplies a chosen architectural direction and acceptance contract; it is intentionally not yet a binary format suitable for mounting.

Qualification shall cover:
- Exact-source portable/native/Windows/Common V2 conformance, golden little-endian vectors, malformed-media fuzzing, bounded parsing and zero writes on refused preflight/read-only inspection.
- Native quick format, repeated format, unformat with post-format writes, release, near-full operation, shared/compressed/sparse/inline data, ACLs, streams, quotas, snapshots and encrypted/mirrored configurations; independent manifests compare every object, reference and logical payload. Inject interruption, torn writes, failed/ambiguous flush and member loss at every publication and release boundary. Native successful undo requires 100% manifest fidelity within the defined failure model.
- Foreign formatter/repartition matrix on disposable images and representative physical media; measure exact overwritten/discarded ranges, retained bytes, verified complete/partial/ambiguous/unrecoverable objects, false positives and source immutability. Include negative controls with overwrite, lost keys and discard. Do not claim a success rate without measured conditions.
- Region/metadata copy loss, stale/contradictory commits, forged/mixed-generation records, missing checksum ranges, nonce/key lifecycle, stale-member fencing, resync interruption, growth/shrink with retained roots and near-full reserve exhaustion.
- Scale at 1, 10, 50 and 100 TiB using sparse test storage where appropriate and real device qualification where required; million-file baseline, fragmented data, random checksum lookup, snapshot count growth, ordinary mount/cache memory, commit write amplification, fsync percentiles, sustained throughput and HDD/NVMe/NAS-like workloads. A full 100 TiB bitmap alone costs 3.125 GiB; ordinary mount must demonstrate memory bounded by configured cache and active workload, not that bitmap. Report actual memory, metadata space overhead and recovery times; record device, software, hardware and configuration. Establish numeric throughput/latency and overhead budgets against measured Format 0.19 and comparable real filesystems before declaring performance acceptance. No durability barrier may be removed to meet a timing target.
- Physical power interruption, endurance, native Windows mounted-driver/Driver Verifier, BOOT and cross-platform exchange are separate evidence classes; simulated faults and historical CI do not substitute. Retain the established ten-minute automatic CI ceiling, dispatch long/destructive qualification separately.

Fix existing correctness defects (mirror divergence latching, untrusted KDF workload, inconsistent feature admission) independently where appropriate; do not wait for a disk-format change to correct them. Preserve the current qualification history rather than carrying its pass status forward to Format 0.20. All enabled features require implementation and appropriate exact-source qualification before being marked complete.

**Design acceptance:** a filesystem in which recovery is a native property of storage identity, immutable roots, explicit ownership, protected publication and independent metadata discovery. The Unformatter is the consumer of that architecture, not a compensating patch for ordinary filesystem design.
