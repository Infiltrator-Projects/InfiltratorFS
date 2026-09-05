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

Current Format 0.18 requires UTF-8 names, sparse extents and the allocation-tree representation. Current formatters enable the established known feature set for inline data, shared extents, paged metadata/extents, symbolic links, hard links, snapshots, object-index tree, directory tree, allocation tree and compressed extents.

A representation is accepted only when its feature bit/version contract agrees with the structure actually stored. A feature bit that contradicts the selected record version is corruption.

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

Object types include directory, regular file, object index, checksum metadata, symbolic link and snapshot catalog.

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
- future portable security-object ID; and
- future portable extended-metadata object ID.

These are not Linux `struct stat` or Windows file-information structures.

Current security and extended-metadata references remain zero until their portable object classes and compatibility contracts are defined.

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

`infilfs-scrub` validates the authoritative reachable graph. `infilfs-forensic` may discover recognizable authenticated metadata physically, but forensic discovery alone is not committed state.

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