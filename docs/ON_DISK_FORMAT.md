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

**Status: revision 4, unified engineering specification, 25 November 2026.** This replaces the earlier 0.20 proposals in this document. It does not change the implemented 0.19 format above. The project is pre-1.0: there is no obligation to mount or migrate an obsolete development format; an older/unknown volume must be recognised/refused without modification. Do not change the format identifier, publish a formatter or claim feature completion until the byte schema and implementation are qualified. The design below is normative intent for the proposed format, with a concrete candidate encoding and state-machine contract in sections 20.13–20.20. Candidate constants are not assigned to production media until schema freeze and executable qualification.

## 20.1 First principles and evidence

One filesystem, one semantic object model, one generation/transaction model, one lifetime/ownership model, one integrity model, one reconstruction model. Recovery is inherent in ordinary storage, not a backup namespace or an after-the-fact scan heuristic. Preserve the original directory hierarchy, identities, exact physical file layout and surviving content after accidental destruction of primary metadata by any formatter. Native quick format additionally guarantees preservation of previous allocations until explicit release under the declared failure model. No software can reconstruct bytes overwritten or discarded on every available copy, or decrypt without keys.

Retain little-endian 4096-byte blocks, stable 128-bit identities, portable names/attributes/security, generation-based CoW, sparse/shared/inline data, IAC1 default compression, SHA-256 logical integrity, snapshots, quotas, online maintenance and native platform adapters. Reject feature-specific parallel allocators, private journals or hidden weakened semantics. Do not add global deduplication, distributed filesystem semantics or application transactions merely because common primitives might permit them.

Evidence motivating changes (historical observations, not current measurements): the repository's FILESYSTEM-PAPER-AUDIT-2026.md identifies per-file checksum cloning and repeated ownership discovery; UNLINK-PERFORMANCE-HANDOFF.md records whole-population scans in old reclamation paths; PERF-019-SYNC-LATENCY.md records a final-sync bottleneck; QUALIFICATION.md records existing 0.19 interruption and mounted evidence. External reference mechanisms, not imported format requirements: [Btrfs extent references](https://btrfs.readthedocs.io/en/latest/dev/dev-btrfs-design.html), [XFS online fsck/reverse mappings](https://docs.kernel.org/filesystems/xfs/xfs-online-fsck-design.html), [Windows format TRIM options](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/format), and [mke2fs discard and deferred initialisation options](https://man7.org/linux/man-pages/man8/mke2fs.8.html). These justify investigation of shared references and recovery damage models, not claims about actual 0.20 throughput or formatter survival rates.

## 20.2 Identity and geometry

Storage-set UUID identifies a single physical allocation/recovery domain; member UUID identifies a backing member independent of enumeration order. Volume UUID identifies a logical filesystem incarnation; object IDs are stable within that volume. Allocation IDs identify immutable content units independently of the file(s) referencing them. Metadata page IDs identify immutable page incarnations; new content gets a new page ID, avoiding generation/ABA ambiguity. IDs are nonzero 128-bit; identity scopes and collision handling are explicit. A set-wide checked monotonic transaction sequence identifies publications, including rollback and unformat. Do not reuse logical allocation/page IDs while any current or retained reference, recovery publication or pending reclaim may exist.

Addresses are member-relative physical byte/block positions with explicit original member and volume geometry; enclosing disk partition offsets and host device paths are not identities. A discovery record identifies its expected member-relative address, set/member IDs and geometry, allowing raw scanning to infer and corroborate an original start offset. Persist committed capacity independently of backing capacity. Use checked 64-bit range arithmetic with wider intermediates or equivalent checks. Define supported minimum and maximum from mandatory metadata protection, reserve, tree depths, byte addressability and platform limits. A 16 MiB minimum is a candidate, not an obligation to weaken protection.

## 20.3 Canonical records, trees and roots

Use one typed immutable page/record family. An envelope includes format, record type/version, header/payload lengths, identity and scope, creating sequence, logical key/range, copy identity, expected physical address, CRC64-ECMA and canonical padding. Logical SHA-256 digest commits identity/type/version and canonical semantic bytes; physical copy address, ordinal, CRC and placement-only fields are excluded. Physical CRC covers the full encoded block with CRC field zeroed. Define domain separators, endian encoding, complete record bounds, mandatory feature profiles and incompatible versions in the byte schema. Reject unknown mandatory semantics and noncanonical encodings before traversing references. CRC and unkeyed digests do not authenticate a malicious replacement of the whole unencrypted volume.

Logical metadata is a keyed persistent tree of typed records: volume/object attributes, namespace edges, file and stream mappings, sparse/unwritten ranges, security principals/descriptors, extensions and quota policies. Directory edges are canonical records keyed by parent and namespace lookup identity, not merely a cache of path strings; object lookup is keyed by scoped identity. Use common CoW balanced tree mechanics (B+ range trees preferred; deterministic radix/Merkle variants where needed). Paged values/records support large names, ACLs, streams and extent lists with explicit bounds. No one-block catalogue cap or linear linked-chain lookup as a required scaling mechanism.

Use the ordinary immutable authenticated B+ tree as the committed membership proof. Leaf records, internal routing keys and child identities/digests form a Merkle DAG; physical copy addresses are excluded from its logical commitment. A scan can recover any missing physical copy by matching its committed page identity and digest. If all copies of an internal page are lost, a mere collection of surviving leaves is insufficient to prove the committed tree's exact membership; recover complete subtrees with surviving proofs, otherwise classify as salvage. Do not introduce a second mutable namespace or promise arbitrary reconstruction of a missing cryptographic commitment. A separately computed canonical semantic export manifest verifies logical equivalence after a tree rebuild, not on-disk commit authority. Tree shape changes can change the root digest without changing semantic file content.

A set checkpoint binds sequence, set/member geometry, active-volume descriptor, retained-volume catalogue, allocation descriptor/location roots, lifetime/space roots, protection/key profiles and semantic root commitments. A volume-state root binds its namespace, object attributes and policy records. Named snapshots reference immutable volume-state roots; retained volumes preserve their snapshot catalogues. Neither snapshot root recursively captures the mutable catalogue or global allocator. Keep stable semantic digest separate from physical layout digest. Checkpoint references for bootstrap metadata include direct, validated physical replica addresses and expected digests, so locating the placement/ownership roots never depends on those roots themselves. The storage-set roots identify the current locations used by all retained semantic roots.

## 20.4 One extent model: data, compression, sharing and protection

A file or named stream is a logical length plus ordered mappings: explicit holes, unwritten reservations, inline bytes, or references (allocation ID, slice offset, length). A data allocation descriptor defines decoded length, stored length, codec/version, integrity tree/root, protection class and encryption domain. Its separate location record describes member-relative physical extents and copy roles. Content descriptors are immutable while referenced; physical relocation updates the location record in one set transaction without changing allocation identity or logical digest. File/stream metadata never stores a second authoritative copy of that location. Location records are essential recovery metadata: untagged payload bytes cannot reconstruct a missing physical mapping by themselves. Preserve them redundantly and bind them to the committed set root.

Use indexed SHA-256 logical integrity on allocation-relative 4 KiB decoded units, shared with the immutable data. The final partial unit is zero padded for hashing only, with exact decoded length independently bound to the descriptor. Define alignment, slice bounds and canonical handling of holes/unwritten data. File logical size and mapping ordering are independently committed; an extent digest alone does not prove the file length or order. Partial EOF, unaligned slices and boundary changes must not disclose bytes outside a file's logical range; materialise a bounded replacement unit where canonical shared coverage cannot be maintained. Reflink reuses both data and checksum roots; clone only mapping paths and increment direct references. No checksum chain copied merely because a different object references the allocation. Whole-file clone cost must not scale with the number of individual checksum records. Partial clones can scale with affected mapping boundaries.

Compression is a representation choice at the same allocation layer. Preserve bounded IAC1 v1 and default adaptive selection when at least one physical block is saved after relevant overhead. Keep LZ4 as a supported non-default representation as required by the existing codec contract. Validate stored length and bounds, decode within a bounded budget, verify logical integrity. A compressed allocation may be shared whole or through explicitly valid decoded slices; mutations replace/materialise affected bounded clusters, not arbitrary slices of encoded streams. Defrag copies encoded bytes without recompressing when policy and authentication allow, verifies the replacement, and republishes location. Holes own no data; unwritten reservations own space but return defined zeroes and never expose old bytes.

Protection is an allocation-level property. Placement uses stable allocation and member IDs rather than the reading file's identity or member enumeration order. Reflink shares only if effective protection and encryption domains are compatible; otherwise copy/re-encode under the requested policy or explicitly refuse. Policy changes on existing content are transactions that replace or relocate allocations; no silent reduction or reinterpretation. Inline data receives structural metadata protection and is eligible only where it satisfies the effective data policy and confidentiality domain. All attributes, streams, ACLs and timestamps remain portable.

## 20.5 Ownership, free space, reclaim and quotas

One set-wide lifetime engine covers immutable metadata, data allocations, snapshots, retired roots, open-but-unlinked objects, prepared transactions, fallback roots and maintenance pins. Use direct incoming edges/backreferences with checked counts, not transitive file or snapshot enumeration. Namespace link counts are distinct from storage reference counts; directory links cannot create forbidden namespace cycles. References from shared immutable tree pages count once per physical logical page incarnation, not once per reachable snapshot. Distinguish strong lifetime edges from non-owning lookup/index entries and semantic identity references; a registry/catalogue lookup must not accidentally keep an unreachable object alive. Design the strong ownership graph acyclic; security/principal sharing, extensions, checksums and stream payloads follow the same model. Scrub independently traces roots/edges to reconcile counts. An inconsistent count cannot authorise free space.

Persist physical ownership ranges (member, start, length, allocation/copy identity, state), a location/identity index and direct reference records as one transactional model. A region bitmap/free-extent summary is a derived availability index: it may be persisted for speed but can never override a contradictory owner or reserve. Use demand-paged sparse range and bitmap representations according to fragmentation; no full-volume bitmap allocation on mount. Exact physical overlap is forbidden between distinct allocations/copies, except authorised multiple logical references to the same allocation; reserved/unavailable/quarantined areas are explicit. Range splits/merges preserve identity and reference semantics, not just counters.

Avoid self-referential allocation accounting: roots and pages of the allocator/location/reference infrastructure are managed by a bounded, explicitly reserved CoW bootstrap arena with direct physical references and checkpointed arena occupancy. They are not inserted as ordinary self-owned records in their own tree. A freeze must specify arena expansion/shrink, old/new arena-root discovery, exhaustion bounds and independent scrub reconciliation of the arena against normal ownership. This is one explicit bootstrap rule, not an independent file allocator. Reserve enough for pessimistic path copying, mandatory redundancy, recovery publication and GC progress before accepting writes. Replenish transactionally. On ENOSPC refuse before damaging current state; emergency space cannot be consumed by ordinary payload.

Deleting a reference queues zero-count allocations for bounded deferred reclamation. The queue, outgoing-edge removal progress and reuse eligibility are durable and idempotent; a crash/retry cannot decrement twice. Keep pending frees unavailable until a committed release; recheck all live, retained, fallback and operation pins before reuse/discard. A root drop must not scan the entire filesystem synchronously. Persist orphan roots for open-unlinked files until final close or recovery; mount recovery must handle those without silently resurrecting them into the namespace. File data, descriptors and typed extensions use the same collector rather than unrelated reclamation algorithms.

Quota policy is portable: stable principal/group/project identities, explicit logical charge and optional physical/retention budgets. Define accounting of hard links (one object per charge domain), reflinks (logical charge per file, shared physical allocation counted once), compression, streams, snapshots, reserved and retired usage. Counters are derived and independently reconcilable; quota enforcement is atomic with allocation/reservations. Native OS quota APIs map onto the common policy. No host-only quota state may silently disappear on unformat or cross-platform access.

## 20.6 Generations, snapshots, transaction durability and NoCoW

All mutation publishes a complete set root atomically. Named snapshot is a root pin; clone is a shared mapping; whole-volume rollback and native unformat are root selection at a new sequence. Old roots and all their dependencies are retained while the promised fallback or user-retention contract applies. Prepared records never establish authority. Restore selected objects by a transaction that preserves exact identities where legal, otherwise creates explicitly mapped identities and resolves names/links/security without overwriting unrelated data.

Reserve and prepare independently in parallel; write payloads, all required metadata replicas, location/ownership changes, semantic commitments and protected discovery dependencies. Flush the required members before writing a new authoritative commit. On a single device the first independently valid durable commit is the crash-authority boundary; successful fsync/administrative acknowledgement requires the full declared redundancy/durability profile, including all required commit copies. Rotate publication so at least one previously committed recoverable path survives until the new one is established; retain its physical dependencies until retired. An ambiguous failure fences mutation until cold authority validation. A candidate may be committed even if its caller received an error; report distinction. Multi-member acknowledgement/fencing is defined separately below. Buffered API completion is not fsync. No success before durability; no relaxation of ordering for speed.

Metadata is always CoW. Data policy may adapt between CoW and eligible in-place writes. Newly reserved/unpublished storage can be written directly. For already committed data, true NoCoW is eligible only with exclusive ownership, no retained/fallback dependency, compatible integrity/protection policy and a proven atomic data+digest update or durable undo/redo protocol integrated with this transaction engine. A normal 4 KiB overwrite plus subsequent checksum update is insufficient. Default to CoW if any condition fails; do not promise fully supported in-place mutation until fault-injection tests pass. Never silently turn off SHA-256, snapshots or recoverability for a speed mode. Avoid a general-purpose second journal unless evidence demonstrates it is necessary; bounded transactional in-place intent/undo records, if adopted, use the same publication and recovery authority.

## 20.7 Recovery-first metadata placement

The ordinary canonical metadata is itself the protected recovery information. No extra independently mutable backup namespace. All semantic records, physical location mappings, roots, essential ownership/reserve state, crypto geometry and required key-header information must remain independently discoverable from ordinary operation. Derived indexes (object/namespace lookup structures, free-space summaries, quota counters, reverse-edge indexes where derivable) can be regenerated; their absence cannot alone defeat reconstruction. Normal mount-critical tree pages are also protected. Copies have the same logical identity/digest and independent physical validation, with copy-specific addresses excluded from semantic identity.

Proposed baseline: three physically separated logical metadata lanes, each sufficient to reconstruct the committed semantic records and physical location mappings without another lane or primary index, plus three separated commit/discovery paths. Lanes are dynamically growing placement groups, not fixed contiguous partitions. They share one logical transaction/record model. File payload is not automatically triplicated; inline bytes inherit metadata protection. A lower-overhead alternative is acceptable only if it meets the same demonstrated damage model; never silently weaken profile for small/full devices. Three-copy count and region profile are candidate engineering parameters; mandatory recoverability is the requirement.

Every lane has independently discoverable commit evidence binding the same semantic and location commitments, geometry, sequence and roots. A raw scan can recover missing addressed copies of tree nodes using immutable identities and committed digests. Loss of all copies of a required proof node limits what can be classified as committed; leaf records without proof remain salvage. Mixed copies must satisfy the same committed root, scope and record digests. Missing essential location records, missing commitments, ambiguous generations or absent data cannot be guessed away. A high generation/valid CRC alone does not prove current membership. If every proof of the latest commit is gone, do not claim a surviving older root is necessarily the latest. Label verified historical state separately from ambiguous salvage.

Place copies according to versioned geometry-aware placement domains; optimise against correlated write/discard regions, not simply equal spacing. A 1 GiB region is a starting candidate, not a requirement. Placement remains independent of workload heuristics; verify separation at format, allocation, defrag, resize and repair. Preserve direct bootstrap addresses and bounded anchor probes for normal mount/BOOT, including changed capacity with surviving anchors. Full-device scans are explicit offline work. Validate exact foreign formatter overwrite/discard maps, partition edits and first-mount background initialisation. Same-disk copies do not protect against whole-device failure or device-wide discard.

## 20.8 One Unformatter and one reconstruction model

There are distinct operations with a common engine: bounded checkpoint selection/healing during mount; structural fsck; deep scrub; explicit read-only offline discovery and reconstruction; native quick-format reversal. They share format validators, root-selection rules and ownership reconstruction, but have deliberately different I/O costs and authority. No hidden full scan in mount/fsck/BOOT; no automatic promotion of an ambiguous graph.

The Unformatter opens a raw device/image exclusively for read-only discovery, identifies old geometry even after partition changes, locates independently valid set/member/volume records and committed lane roots, verifies membership and checksums, reconstructs namespace, extent mappings, security, streams, snapshots and physical ownership, and rebuilds derived indexes. It must traverse surviving committed proof paths; it does not establish exact membership by selecting the most recent record for each key. It uses current placement mappings for retained allocations, not stale physical addresses. For encrypted devices first discover/authenticate the container headers and obtain authorised keys; raw ciphertext is not plaintext metadata. Verify per-object metadata and logical data separately; preserve links, exact names and logical holes. Report verified complete, partial with explicit missing logical ranges, ambiguous and unrecoverable; never turn missing data into a sparse hole. Evidence without a committed root is salvage, not a guaranteed original filesystem.

Separate-destination export is nonmutating by default, with manifest, source geometry, selected commit, copy provenance, losses, and destination capability report. Do not follow recovered symlinks/reparse points or execute recovered files. In-place reconstruction of foreign damage rebuilds structural metadata around surviving payload positions without gratuitous source-internal data movement. Require a complete verified graph, independent verified backup/export, exclusive target identity, safe staging/reserve, user authorisation, crash-safe publication and scrub; if insufficient, refuse writable repair. Foreign geometry restoration must not overwrite live unrelated partitions. Recovery does not replace backups.

Native quick format on a valid supported set is a privileged transaction creating an empty new volume UUID while pinning the former volume descriptor and snapshot catalogue. Same allocator, integrity and recovery lanes; no data zeroing, discard, container reinitialisation or silent release. Report retained usage and actual free space; refuse if the new graph and reserve do not fit. Repeated formats retain distinct roots until explicit release. The common Unformatter selects/validates that retained root, republishes it under its original volume UUID at a new set sequence and normally retains the displaced active root, preserving post-format files. No physical payload copy is needed on an intact native set. Healthy native reversal needs explicit exclusive authorisation and complete verified roots, not an unnecessary external-copy prerequisite; damaged/foreign repair requires verified independent export/backup as above. All formatter entry points share target-bound preflight and zero-write refusal; blank create, recoverable quick format, retention release and destructive replacement are distinct. Never silently fall back to destruction.

## 20.9 Security, encryption and replica placement

Preserve portable ordered deny-aware ACLs, stable principals and opaque adapter metadata. Shared immutable security descriptors use the same strong-reference and deferred-reclaim contract as other metadata. Preserve exact policy on recovery; do not widen privileges because a destination cannot represent them. Offline unencrypted media lacks confidentiality; CRC/digest validates accidental integrity, not adversarial authorship.

Encryption container has independent version and geometry. Distribute authenticated wrapped-key headers in independent placement domains with enough copies to meet the recovery profile; no plaintext keys or passphrases stored in recovery metadata/logs. Bootstrap headers must be discoverable without ordinary filesystem metadata or the allocator; validate bounded KDF before derivation and authenticate parameters. Bind data to container identity, allocation/domain and a stable authenticated record address/identity; specify relocation semantics (re-encrypt if physical binding changes), nonce uniqueness across concurrency/crash/reformat, per-key usage budget and rotation. Retained roots keep needed keys. Authenticated allocation-aware unwritten state may avoid full-volume zero initialisation only when missing/tampered ciphertext cannot be mistaken for unwritten zeroes. A format/container replay cannot be detected absolutely without a trusted external freshness source. No bespoke cryptographic primitive; use vetted AEAD/KDF and independent review.

Replication uses persistent member UUIDs, membership epoch, placement map, committed root evidence and durable dirty/resync state. Same allocation/location engine on one or many members; metadata lane placement and per-data protection share explicit fault-domain policy. Synchronous protection acknowledges only after all required members' data/metadata/commit barriers; define exact cross-member commit/authority protocol before implementation. Partial or ambiguous writes fence conflicting mutation immediately. Degraded writes are a separate explicitly qualified mode requiring exclusive authority and durable stale-member fencing; isolated two-way mirrors cannot safely elect both sides writable. Repair validates expected identity and digest, never trusts highest sequence alone; progress is durable and resumable. No new distributed filesystem requirement.

Retention release only drops roots and allows eventual reuse. Destructive format and secure/cryptographic erase require distinct explicit intent. Purge includes every applicable key/recovery copy; physical erasure on SSD/remapped hardware is limited to verified device guarantees. Do not automatically revive purged generations. Old recoverable records on unencrypted media are not a guarantee of privacy or deletion; disclose these semantics.

## 20.10 Namespace, platform and maintenance

Keep validated byte-exact UTF-8, 1023-byte general names, optional removable-name profile, original spelling and stable versioned comparison. Preserve current ASCII casefold semantics if provided; full Unicode folding is a new optional pinned version and collision policy, not silent reinterpretation or a prerequisite to recovery. Keep hard links, symlinks, four nanosecond timestamps, inline data, sparse extents, named streams and opaque typed extensions. Remove artificial one-block metadata ceilings using common paged values with explicit resource bounds; maintain exact security principal/ACL semantics and adapter preservation. A native adapter may refuse unsupported access but must not silently discard metadata.

Defrag/compact/repair relocate physical allocations through the location transaction; preserve semantic content, checksums, reference identity, policy and user-visible timestamps. Retained roots resolve current valid location by allocation ID; do not pin old physical addresses forever unless a publication/fallback still requires them. Growth changes geometry without rewriting historical volume-state roots. Shrink checks all active, retained, bootstrap, pending and fallback locations, relocates safely or refuses; physical truncation follows durable discovery and ownership update. Stable online scrub pins a generation and placement view, verifies logical data, copy separation, reference edges and accounting; it must coordinate relocation/reclamation. Automatic fsck repair restricted to deterministic validated copies; explicit reconstruction is a separate operator-authorised operation. Preserve existing single fsck executable with --scrub, no separate redundant scrub binary.

One canonical serialized schema, format validator, common C/C++ contracts and golden vectors for portable core, native Linux, Windows and freestanding Common V2 reader. Native OS APIs, page cache, mmap, lifetime and security projection stay adapter responsibilities. Common V2 BOOT reader has bounded read-only discovery and integrity semantics; unsupported features explicitly fail. No V1 or FUSE runtime fallback. One format authority does not mean forcing firmware to implement writer/GC machinery. Capability gates, not divergent interpretations, define supported platform operations.

## 20.11 Performance as a design invariant

Repository evidence shows real unlink/reflink/commit hot paths; avoid introducing equivalent global scans in the new design. Ordinary mount memory depends on bounded caches and active workload, not volume capacity. Regional free-space summaries, sharded reservations, dirty path updates, immutable validation caching and deferred bounded GC. Use measured allocation/fragmentation tradeoffs; no whole-volume scans in ordinary foreground operations or whole-inventory copy per commit. Snapshot and volume retirement are root operations independent of file count, excluding explicit validation/export and deferred reclamation. Reflink and relocation must not copy per-block checksum metadata or walk unrelated files. Shared-reference accounting and self-hosting reservations are bounded and tested.

Retain the existing filesystem-wide native Linux max(1, online physical cores minus one) CPU-heavy work budget, dynamic workload/memory/latency admission, bounded per-worker IAC1 and hardware-accelerated SHA-256 with portable equivalent output. Parallelise compression, checksums and independent CoW preparation; group eligible commits and minimise global coordination without merging incompatible durability requests or weakening required flush order. Optimise physical replica placement/batching for HDD and NVMe while maintaining separation. Record metadata overhead, data protection cost, fragmentation, dirty metadata/write amplification and fsync tail latency. For scale context, one full bit per 4 KiB block costs 3.125 GiB at 100 TiB; avoid allocating it at mount.

## 20.12 Specification freeze and qualification

This revision chooses a coherent architectural model, not arbitrary binary constants. Before code changes, freeze exact schema offsets, keys, tree/semantic commitment algorithm, page/record IDs, digest domains, bootstrap arena accounting and reserve formulas, location and reference transitions, compressed/tail checksum coverage, physical placement profiles, commit slots/ordering and cryptographic/membership protocols. Prove no circular dependency between allocator, placement, commit and reconstruction roots; independently model strong-edge GC and NoCoW failure semantics. If a candidate is too expensive, simplify the mechanism while meeting the same semantics rather than dropping a guarantee. Record measured decisions in DECISIONS.md.

Acceptance:
- Exact-source cross-platform golden images, malformed media/fuzz, overflow/unknown features, read-only immutability, zero-write refusal, existing namespace/data/security/maintenance semantics and Common V2 bounded reads.
- Model and actual-writer crash/tear/ambiguous-flush matrix at reservation, payload, location, ownership, replica and commit boundaries; include near-full reserve, delayed reclaim, open-unlink, repeated rollback/format, failed resync and physical shrink. Prove old-or-new authority and independently trace reference counts; no leaked/reused retained blocks.
- Foreign quick-format/repartition tests on ordinary populated volumes: real Windows/Linux/NAS formatter versions and options, HDD and discard-capable storage, exact physical overwrite/discard trace; measure first mount separately. Inject destruction of primary indexes, any two full metadata lanes under proposed profile, mixed-record damage, missing location maps and stale/prepared commits. Require exact namespace, physical extent layout, metadata and logical payload manifest fidelity whenever committed essential records, payload and keys survive. Otherwise truthful loss classification. Native repeated format/unformat with new writes must preserve both roots and full fidelity under declared conditions. Never claim universal recovery.
- Test shared allocation/checksum behavior across reflink slices, snapshots, hole punch, partial EOF, compression, policy changes, defrag, resize, scrub and recovery; verify old physical locations released only after safe publication and no duplicate checksum work for clones. Test encryption headers, nonce/key lifetime and replay limits; multi-member stale fencing and repair.
- Compare against measured exact-source 0.19 baseline and representative ext4/XFS/Btrfs (workload, device, durability and data protection settings disclosed). Target no foreground whole-population scan, cache memory independent of capacity, bounded GC/transaction work and clone/relocation cost tied to changed mappings rather than file population. Evaluate 1/10/50/100 TiB sparse-image scale plus real devices, million-file workloads, near-full fragmentation, random 4 KiB fsync percentiles, sequential throughput, metadata-heavy and mixed HDD/NVMe/NAS workloads. Freeze numeric overhead/latency/throughput budgets from measurements before production acceptance. No invented benchmark improvements.
- Physical power-loss/endurance, native Windows mounted/Driver Verifier, native Linux root boot, BOOT/Common V2 and real platform exchange are independent qualification classes. Preserve ten-minute automatic CI limit; dispatch destructive/long tests separately. Historical 0.19 evidence remains historical.

**Acceptance principle:** the filesystem's ordinary records, immutable generations, ownership, integrity and physical placement make recovery a natural consequence of its design. Native quick-format undo and reconstruction after foreign formatting use the same structures; neither is an independent backup mechanism. The proposed format must earn its complexity through measured real-world reliability, performance and explainability.

## 20.13 Candidate wire contract and encoding discipline

This section selects a concrete starting point for the byte-schema model. The exact candidate envelope below can be used by an isolated encoder/decoder prototype; it is not an instruction to write 0.20 media with existing production tools. No format or feature ID is registered yet. Every persistent integer is unsigned little-endian unless the typed payload explicitly defines a signed field. UUIDs are opaque 16-byte sequences, compared lexicographically as stored; do not serialise native GUID structures. Block positions are 64-bit indices in the relevant member's 4096-byte filesystem address space. Check multiplication and addition before storage I/O. No compiler bitfield, pointer, size_t, alignment or struct-padding convention is persistent.

Candidate common envelope (one 4096-byte page, all offsets decimal):

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 8 | Format-family/page magic, exact value allocated at schema freeze |
| 8 | 2 | Major format |
| 10 | 2 | Minor format |
| 12 | 2 | Page kind |
| 14 | 2 | Page schema version |
| 16 | 2 | Header length (candidate 192) |
| 18 | 2 | Required flags |
| 20 | 4 | Payload byte length |
| 24 | 16 | Storage-set UUID |
| 40 | 16 | Scope UUID (zero only for explicitly set-global kinds) |
| 56 | 16 | Immutable page ID |
| 72 | 8 | Creating transaction sequence |
| 80 | 32 | SHA-256 logical page digest |
| 112 | 16 | Physical member UUID |
| 128 | 8 | Expected member-relative block |
| 136 | 2 | Copy/lane ordinal |
| 138 | 2 | Physical encoding flags |
| 140 | 4 | Reserved, zero |
| 144 | 8 | CRC64-ECMA physical checksum |
| 152 | 40 | Reserved, zero |
| 192 | 3904 | Payload followed by canonical zero padding |

This gives 3904 payload bytes before typed page headers and record tables. Encode fields individually through shared C/C++ helpers; compile-time assertions and golden bytes verify offsets. CRC64-ECMA uses polynomial 0x42F0E1EBA9EA3693, initial zero, no reflection, final XOR zero; the physical checksum field is zero during calculation over all 4096 bytes. The logical digest is SHA-256 over a versioned domain separator, length-prefixed canonical logical header fields and typed semantic payload, excluding physical copy information and CRC. Candidate domain tags are ASCII INFS20/PAGE, INFS20/COMMIT, INFS20/CONTENT and INFS20/MANIFEST, each length-prefixed; the freeze must fix precise preimages and vectors. Tree child logical reference = child page ID plus digest; direct physical locator entries are separate typed address fields excluded from that logical digest. For physical location records the stored data-member addresses are semantic values and MUST be included in their digest; excluding copy-specific metadata placement does not exclude file-data locations. Validate physical CRC, expected address and identity, logical digest and typed canonical encoding before use. The enclosing encrypted-container integrity applies to stored ciphertext independently.

Candidate typed record framing is a fixed 8-byte prefix (kind u16, flags u16, key length u16, value length u16), followed by exact key and value bytes; no implicit padding. Oversized keys/values use explicitly typed paged continuation records with length and digest, not an unbounded parser allocation. Tree pages declare level, record count, occupied length and ordered slot offsets. Reject duplicate keys, unsorted keys, overlapping slots, unreachable payload bytes, noncanonical slack, wrong child level, wrong scope, cyclic child references and keys outside the inherited range. Numeric key fields compare by decoded numeric order; opaque IDs and UTF-8 lookup bytes compare lexicographically. Do not assume that little-endian numeric byte ordering is numeric ordering. Freeze page capacity using actual encoded worst-case keys (including 1023-byte names), direct replica references and minimum split occupancy; do not invent fanout from a nominal pointer size.

## 20.14 Record families and reference semantics

| Record family | Key / scope | Essential meaning and owning references |
|---|---|---|
| Set commit | set, sequence | Root identities/digests, geometry, protection profile and publication evidence |
| Volume descriptor | volume UUID | Label, policy, active or retained state root, snapshot catalogue |
| Volume-state root | immutable state ID | Roots of canonical object, namespace, stream and policy trees |
| Object | volume UUID, object ID | Type, attributes, logical size, security and extension references |
| Directory edge | parent object ID, canonical lookup name | Exact original name, target type/ID; a semantic link, not a second owning reference to the same object record |
| File/stream map | object/stream identity, logical start | Kind, length, allocation ID and slice or inline data |
| Allocation descriptor | set, allocation ID | Immutable content geometry, codec, digest root, protection and key domain |
| Allocation location | set, allocation ID | Versioned current physical member/range copies; binds exact layout |
| Physical range owner | member UUID, block start | Length, allocation/copy identity, state |
| Direct reference | target ID, source ID, edge ordinal | Unique strong edge; checked count derived from edges or maintained atomically |
| Principal / descriptor | volume scope, principal/descriptor ID | Portable security identity and ordered ACL, shared descriptor content |
| Extension / stream | owner scope, type/name/ID | Typed values, opaque preservation, paged payload |
| Snapshot / retired root | catalogue scope, stable ID | Pinned immutable state/descriptor, retention metadata |
| Orphan / reclaim / reserve | set, stable entry ID | Open object lifetime, resumable release work, unavailable ranges |
| Membership / key profile | set, epoch or key/domain ID | Required policy and authenticated container/membership bindings |

Object trees own reachable object records through their page structure. A directory edge is a logical namespace reference; stored link counts and reachability must agree, but it does not create an extra physical owning edge to the same record. Remove the object from the active object tree when its last namespace link and open lifetime end; older roots preserve the old object. Object records own their typed payload roots. Allocation registry, physical-location and reverse-reference lookup entries are non-owning with respect to payload lifetime; an entry must not keep the allocation alive simply because it exists. Allocation descriptors remain accessible while any strong mapping or protection pin exists. Strong edges are directed from roots to immutable pages to owned payloads, never back to parent or lookup table. Descriptors own checksum/paged content roots and security descriptors own required principal records; validate acyclicity.

Each identity has a defined incarnation and scope; references to an immutable page or descriptor include expected digest. A physical location change retains allocation identity but creates a new version of the location record. Active and retained semantic roots resolve allocation IDs through the selected set commit's location table; a read or scrub pins that set view, not only a namespace root. Direct physical replicas of metadata tree pages can be discovered by parent locators or offline scanning; metadata relocation is a CoW path update, or a separately qualified indirection mechanism. Do not promise that arbitrary metadata relocation is constant time merely because file-data allocations are indirect.

## 20.15 Extent operations and integrity

Define data-map coverage in bytes: ordered nonoverlapping half-open ranges within logical size; adjacent compatible ranges may merge. Holes are explicit or represented by a single documented absence rule, not both ambiguously. Unwritten reservations reference reserved physical ranges but have zero-read semantics; initialise and verify new payload before converting them to written mappings. Inline content is a bounded object/stream value with a logical digest. Allocation descriptor identity is not a content hash for deduplication.

Initial implementation uses allocation-relative 4096-byte hash units: SHA-256 over exactly 4096 decoded bytes, with zero padding of a partial terminal unit; actual decoded length is separately bound in the descriptor. Indexed checksum pages commit unit number and digest. Validate slice arithmetic, exact read length and zero handling; an allocation must not expose data beyond the mapped logical slice. Prefer aligned ordinary shared slices; unaligned boundary mutations materialise replacement units. Compressed cluster decoding validates stored byte count, codec version, declared decoded bound and exact consumption rules before logical verification. Compression selection uses actual stored blocks under the same protection policy.

Operation contracts:
- **Write / truncate / hole punch:** reserve, prepare replacement content and checksums, change affected map paths, update direct edges; old allocations remain pinned until safe release.
- **Whole-file reflink:** create a new object with a shared immutable mapping root and direct root edge, when policy permits. No per-block checksum cloning. Partial clone changes only intersecting map ranges and necessary boundary units.
- **Defrag / repair:** stage an equivalent physical copy, read/verify it, publish new location and physical owner records together; keep old locations protected for fallback and pinned readers. No file timestamp or logical digest changes.
- **Snapshot:** pin a complete volume-state root at one sequence. Snapshot creation does not walk all files or allocate duplicate ownership bitmaps. Retained volume catalogue owns its snapshot catalogue; a named snapshot does not own the mutable global catalogue.
- **NoCoW:** first implementation may write newly reserved, unpublished blocks directly but uses CoW for already committed data until an integrated durable data+digest update model passes power-cut tests. This is a deliberate conservative implementation gate, not a promise that every existing in-place optimisation transfers unchanged.

## 20.16 Self-hosting, transaction and reclamation state machines

The candidate bootstrap design uses explicitly reserved, append-allocated CoW metadata segments per recovery placement group. The set commit carries direct replica references to the segment directory and allocator roots. The segment directory records member-relative extent, append frontier and segment identity. Segment-directory pages are themselves allocated from the arena. Avoid recursion by not inserting arena pages as ordinary per-page ownership records into the structure they implement. Instead, arena ranges are reserved in the physical ownership map, and exact arena page reachability is independently traced from committed/fallback roots. A transaction consumes a private append reservation, then publishes new frontiers and roots together. An interrupted uncommitted suffix is not authoritative; reuse only after authority is established and no retained/pinned root can address it. Encrypted rewrites require fresh nonce discipline even for abandoned writes.

A segment is returned to ordinary free space only after an explicit evacuation/reachability proof, exclusion from every current/fallback/pinned root, and durable publication. Expand using pre-reserved emergency space in existing arena segments to record newly reserved nonoverlapping ranges; do not allocate new space to escape a reservation failure after modifying authority. Segment directory and frontier update must fit the pessimistic transaction budget; if not, refuse before write. Establish a bounded maintenance algorithm for reclaim/evacuation and test that near-full reserve always permits release progress or a clear read-only refusal. This is a candidate mechanism to model, not proof that every workload fits a particular reserve size.

| Transaction phase | Authority | Required action / interruption result |
|---|---|---|
| OPEN / RESERVED | previous commit | Acquire writer/target authority; reserve all required space and references; no destructive writes |
| PREPARED | previous commit | Write new payload, pages, replicas and all ownership/location/arena updates privately |
| DATA_DURABLE | previous commit | Required member barriers succeed; candidate dependencies complete |
| FIRST_COMMIT | new commit if valid and durable | Publish one authoritative commit record; ambiguous errors fence mutation |
| PROTECTED | new commit | Publish and flush remaining required commit/discovery copies; only now acknowledge declared durability |
| RETIRE | new commit | A later transaction may retire fallback/old locations when no pins require them |
| REOPEN | selected validated commit | No writes until authority established; read-only never heals |

For mirrored storage FIRST_COMMIT is insufficient to claim complete protected acknowledgement. The initial multi-member profile requires exclusive ownership of the set, all required members available, matching prepared root commitments flushed on required members, and a common commit identity published to them. An incomplete commit is an uncertain transaction; do not allow another writer until all required member evidence is examined. Missing members or ambiguous authority permit explicitly qualified read-only inspection only, not automatic degraded writes. Freeze exact member proof/acknowledgement and repair rules before enabling this profile.

Reclamation states are LIVE, PENDING_ZERO, DRAINING, RELEASABLE, FREE (plus QUARANTINED and RESERVED). Removing the last strong edge atomically queues PENDING_ZERO; child-edge removal and cursor advance are committed together in bounded batches; after all edges are removed, no root/pin references the record and fallback is retired, publish RELEASABLE/FREE with physical owner removal. Never decrement twice on restart or reuse a pending range. Scrub independently traces the DAG and verifies reference records/counts and owner ranges; disagreement quarantines space and fences unsafe mutation. A free-space hint never overrides ownership. Runtime reservations and reader pins must be coordinated with publication; persisted roots, not lost process memory, determine cold recovery.

## 20.17 Recovery profile, geometry and proof

Use the three-lane baseline as a testable protection contract, not a promise of survival under arbitrary damage. The layout prototype must compare candidate region sizes and placement choices using measured overwrite/discard traces, metadata locality and free-space fragmentation. Initial candidates: 64 MiB, 256 MiB and 1 GiB placement regions; scale to smaller geometry profiles only if required separation and reserve remain demonstrable. Three lanes do not mean three contiguous thirds of the device. Each essential logical page and commit must have an independently locatable copy in each required lane, with no common physical block. For ordinary B+ trees, a lane's surviving root and reachable copies supply membership proof; derived indexes may be rebuilt. Loss of all copies of a proof node is outside full-reconstruction guarantee even if unauthenticated/stale leaves survive. File data protection is separate; report actual copy policy.

Use a fixed bounded normal discovery profile with initial probe addresses and redundant location records for relocated anchors. A discovery anchor is a hint to commit slots, not authority. For the candidate commit slot design keep old/new independently writable slots in each lane, a complete root set with expected digests and direct physical references, sequence, geometry, prior commit digest, profile, member epoch and physical CRC. No separate unflushed mutable commit flag. On geometry change write replacement discovery hints while retaining a path to old authority; publish new commit and refresh remaining hints before allowing physical truncation. Freeze exact bootstrap offsets after examining actual formatter damage; do not assume block zero, midpoint or tail survive.

The offline Unformatter performs:
1. Open immutable image/read-only exclusive source, enumerate geometries and container headers, scan independently valid page/commit records with bounded memory or an external index on another device.
2. Group by set/member, original geometry and format profile. Select a provable commit; distinguish historical evidence from latest-known authority and ambiguity. Never select per-key newest records as a substitute for root membership.
3. Resolve each required tree child by expected identity and digest through direct locators or the scan index; verify range, scope and consistency. Construct directory/object/extent/security/snapshot graph and physical owner map. Derive missing lookup/availability indexes only where essential proof survives.
4. Verify logical payload per allocation once, map valid/missing units to each referencing file and stream. Record metadata and payload completeness separately. A missing file-data range is not a sparse hole.
5. Export a deterministic manifest: set/volume/root identity, commit proof, exact names and edges, attributes, ACL/stream/extension data, allocation ID and physical extents, logical sizes and digests, copy provenance, gaps, ambiguities and destination limitations. No writes to source.
6. Healthy native unformat selects a retained volume descriptor and republishes it through the same transaction engine. Foreign in-place reconstruction additionally requires complete proof, verified independent backup/export, exclusive target/geometry authority, safe staging, no overwrite of unrelated partitions and post-publication scrub. Original surviving payloads may remain at their physical addresses. Inability to satisfy any prerequisite means read-only export/salvage, not forced repair.

Define successful native quick format as new UUID/empty namespace, unchanged set identity, explicit retained prior root and protected dependencies. Subsequent writes cannot use retained ranges. Reversal preserves original UUID/objects and by default retains displaced new root. No expiry without explicit release; space usage is disclosed. Native reversible format is a normal root transaction, not a new recovery data structure.

## 20.18 Implementation sequence and acceptance gates

Implementation order follows dependency order, not product feature order. Each stage must have its own failure tests before later stages can rely on it.

| Gate | Deliverable | Required exit evidence |
|---|---|---|
| S0 | Isolated schema/geometry prototype; exact endian encoding and malformed input parser | Golden byte vectors, size/overflow/unknown feature tests; no production formatter |
| S1 | Commit/discovery and self-hosting arena model | Exhaustive small-state interruption exploration, reserve and segment-rotation proof, old-or-new cold reopen |
| S2 | Canonical trees, stable allocation descriptors, location/owner/edge engine | Independent reference tracer, fragmentation and near-full tests, crash-safe GC and relocation |
| S3 | Portable single-device filesystem | Namespace/permissions/streams, inline/sparse/compressed data, reflink, quota and snapshot parity; actual-writer fault tests |
| S4 | Recovery placement and common Unformatter | Real foreign formatter traces, loss of lanes/primary indexes, exact independent manifests, native undo with later writes, source immutability |
| S5 | Native Linux, Windows, Common V2 / BOOT and maintenance | Same golden media and corruption behavior, native mount/root-boot, defrag, resize and independent platform qualification |
| S6 | Encryption and multi-member profiles | Cryptographic review, header/key/nonce tests, member authority/fencing and physical recovery; no unqualified degraded mode |
| S7 | Performance and production acceptance | Exact baseline comparison, physical power-loss/endurance, measured placement and memory/latency/throughput budgets, full regression evidence |

S0–S2 use isolated disposable media; keep installed 0.19 and historical tests intact. Single-device unencrypted format is the complete baseline, not dependent on a mirror, firmware or encryption implementation. Profiles may be developed in parallel when their contracts are fixed; feature admission must refuse unsupported profiles. A release cannot silently remove a qualified existing feature or advertise parity until its corresponding 0.20 gate passes. No user data migration is implied by this design. Historical 0.19 evidence remains version-scoped.

## 20.19 Measurement and engineering decision register

Use repository evidence as the initial baseline, not as fresh measurements. The historical unlink and research audit motivate direct edges and shared integrity; PERF-019 motivates short coordinated publication and I/O accounting. Confirm exact source/device configuration when collecting new data. For every comparative test record CPU, memory, storage model and sector geometry, cache/flush settings, filesystem and formatter versions/options, capacity/occupancy, dataset seed, mount options, compression/encryption/protection profile and source SHA. Compare equivalent guarantees; a faster unprotected mode is not evidence of superior protected performance.

Record p50/p95/p99 and maximum latency for 4 KiB synchronous writes, sequential throughput, random mixed I/O, million-file create/delete/clone, snapshot/rollback, defrag/resize, normal mount memory and cold offline recovery time. Measure payload and metadata bytes separately, actual writes/flushes per operation, metadata replication overhead, physical fragmentation and deferred-reclaim backlog. Include low-memory and near-full volumes, HDD/NVMe and representative NAS storage; use sparse test images for large addressability/scale without claiming physical endurance. Test quick format and first mount separately with discard enabled/disabled where supported. For real formatter qualification use disposable images/devices and preserve exact write/discard traces and before/after manifests. No real user data destructive testing.

Analytical candidate overhead: a 32-byte SHA-256 digest per 4096-byte unit is 0.78125% of decoded data before framing/index overhead. Three copies of those digests alone are 2.34375%; at 1 TiB of dense uncompressed data that is 24 GiB before tree pages, other metadata, reserves and fragmentation. This is arithmetic, not measured filesystem overhead. Reflinks reuse checksum pages; compressed data's overhead relative to stored bytes can be higher. A 192-byte envelope is 4.6875% of a metadata block, before typed page overhead. Record the actual size/latency consequences of three lanes versus alternatives rather than choosing redundancy solely by intuition.

Outstanding freeze decisions: exact magic/feature/record IDs; complete field encodings and tree-page split/capacity; arena segment/reserve formulas; exact anchor/commit offsets and region profile; replication/crypto wire details; quota policy defaults and resource bounds; measured numerical performance/overhead limits. Candidate envelope and state machines are available for prototypes, but these decisions are not asserted final or tested. Record chosen alternatives, counterexamples and measured tradeoffs in DECISIONS.md. No binary compatibility or benchmark claim until the evidence exists.

## 20.20 Final design rule

A feature is accepted only when it follows from the canonical objects, immutable roots, shared allocation integrity, single ownership model and protected committed storage. Recovery should preserve the normal filesystem rather than maintain a parallel one. When a proposed shortcut needs an unrelated allocator, ownership scan, backup namespace or private durability authority, reconsider the design before writing code. Keep correctness and physical recovery limits explicit; make the implementation as simple as the proven requirements permit.
