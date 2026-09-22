<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS On-Disk Format 0.18

This document explains the persistent Format 0.18 model. Exact packed field order, sizes, constants and feature identifiers in `include/infilfs/format.h` are the byte-level authority. Architectural intent lives in `ARCHITECTURE.md`; feature completion and qualification do not belong in this specification.

Pre-1.0 development accepts the current development format only and does not promise readers or migrations for superseded development formats.

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

Those positions are then recorded in the checkpoint and become part of committed filesystem geometry. Readers use the recorded checkpoint locations; they do not recompute them from current backing-device capacity.

The committed filesystem size may be smaller than the physical backing device. Online resize may preserve existing valid checkpoint locations or relocate secondary checkpoints when the new committed geometry requires it. Block 0 remains the bootstrap checkpoint.

A checkpoint records the generation, committed total/free block accounting, allocation-tree root, object-index root, namespace root, checkpoint locations, filesystem UUID, root object identity, feature sets, label and integrity metadata.

Filesystem UUID, root object identity and generation are nonzero.

Each physical checkpoint copy is independently validated for magic, exact Format 0.18 identity, header/block geometry, feature compatibility, canonical reserved bytes, checksum and recorded checkpoint-position consistency before its referenced graph can be considered.

Recovery considers candidate checkpoints in descending generation order and selects the newest complete structurally valid committed graph. Structural corruption may justify fallback to an older committed generation. External I/O failures, unsupported features, memory failure or other operational errors must not be silently reclassified as corruption merely to force fallback.

## 3. Feature compatibility

Format 0.18 uses separate compatible, read-only-compatible and incompatible feature classes.

- Unknown incompatible bits prevent open.
- Unknown read-only-compatible bits prevent writable open.
- Unknown compatible bits may be ignored according to their contract.

Current Format 0.18 requires UTF-8 names, sparse extents, the allocation-tree representation and Unicode-name policy v1. Unicode-name policy v1 validates UTF-8 but performs no implicit Unicode normalization: namespace components are preserved exactly as supplied. Unflagged volumes compare those bytes exactly. The optional `INFS_INCOMPAT_CASEFOLD_V1` policy changes lookup identity by folding ASCII `A` through `Z` to `a` through `z`; every other UTF-8 byte remains identity-significant. Original spelling is retained in directory entries. The fold is deliberately versioned and independent of host Unicode tables, so any broader Unicode case-fold or NFC/NFD policy requires a new feature/version identity rather than silently changing lookup semantics. Current formatters enable the established known feature set for inline data, shared extents, paged metadata/extents, symbolic links, hard links, snapshots, object-index tree, directory tree, allocation tree and compressed extents.

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

Format 0.18 persists that bitset as a checkpoint-rooted copy-on-write allocation tree rather than a monolithic bitmap image.

Allocation leaves use `INFSAL01`; branch pages use `INFSAB01`. Allocation pages carry generation, logical index, level, entry count, payload length and CRC64 metadata.

The current packed allocation-page header is 72 bytes, leaving 4024 payload bytes in one 4096-byte page. Each leaf therefore represents 32,192 allocation bits except the final partially used leaf. Branch payloads contain little-endian 64-bit child block pointers with fanout 503. Format 0.18 fixes the allocation root at level 3.

The checkpoint stores the allocation root and leaf count. The declared leaf count must agree with committed `total_blocks`. Missing, duplicate, out-of-range, malformed or checkpoint-overlapping allocation pages are corruption.

Allocation pages are bootstrap metadata, not persistent namespace objects, and have no object IDs. Unchanged pages may be shared by later committed generations, so a referenced page generation may be older than the selected checkpoint but never zero or newer than that checkpoint.

Transactions maintain the authoritative allocation state plus rebuildable runtime free-extent acceleration. Publication rewrites only affected allocation leaves and the required replacement branch paths/root. Whole-volume bitmap cloning or whole-bitmap publication is not part of Format 0.18.

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

Current Format 0.18 uses a generation-aware radix/tree representation. Directory entries refer to persistent object IDs rather than physical object blocks, so metadata relocation changes the index rather than every namespace reference.

Index tree pages are independently bounds/allocation/owner/generation/checksum validated. Entries require nonzero unique object IDs, valid target blocks, matching object identity/type and canonical flags.

The root object must appear exactly once and must agree with the checkpoint's recorded root identity and root block relationship.

## 7. Common attributes

Portable common attributes contain:

- logical size;
- link count;
- portable flags;
- birth/access/modification/metadata-change timestamps;
- portable security-object ID; and
- future portable extended-metadata object ID.

These are not Linux `struct stat` or Windows file-information structures.

The security reference may be nonzero when the portable-security
incompatibility feature is enabled.
The extended-metadata reference remains zero until its portable object class and
compatibility contract are defined.

A POSIX compatibility record stores current mode/UID/GID information for adapter use. It is compatibility metadata, not persistent object identity or the final portable security authority.

## 8. Directories and names

Current directory heads reference checksummed directory-tree pages. Hashing selects a storage path, but exact UTF-8 name bytes remain namespace identity and comparison is byte-exact/case-sensitive.

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

Small non-empty files may store their payload and logical SHA-256 digest in the file metadata object and own no separate data blocks.

Current `INFS_INLINE_DATA_MAX` is 3840 bytes. Growing beyond the inline bound promotes the file transactionally to extent-backed representation. Shrinking may return to inline representation when the implementation chooses a valid canonical form.

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

Format 0.18 does not require snapshot-aware resize migration. An implementation may conservatively reject resize while retained snapshots exist.

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

A structurally valid Format 0.18 graph requires:

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

For Format 0.18 maintenance, use these sources in order:

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
