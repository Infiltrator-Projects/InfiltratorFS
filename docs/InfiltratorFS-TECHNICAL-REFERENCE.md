<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS engineering reference

## Purpose and specification authority

InfiltratorFS is an independent platform-neutral filesystem. This teaching reference is based on the repository at 6ec9ff0470fc484dd4c579494bdc6bf3e915dcbb. Implemented format 0.19 and proposed format 0.20 are deliberately separated. The exact byte contract is ON_DISK_FORMAT.md together with include/infilfs/format.h and locator.h. The 0.20 candidate schema is not the production format. Existing user guide, design papers and validation records remain intact; this reference supplements them without becoming a second normative format or status authority.

## Format 0.19: geometry and identity

Blocks are 4096 bytes and integers are little endian. Object and filesystem identities are nonzero 128-bit values. Minimum media is 4096 blocks (16 MiB). Initial checkpoints are blocks 0, floor(N/2), N-1. Fixed locators at 1 and 2047 remain reserved during resize and throughout retained generations. Committed capacity may differ from physical capacity. Example: 16384 committed blocks occupy 64 MiB, with checkpoints at 0,8192,16383. Never recalculate checkpoint placement from a larger physical backing device.

## Checkpoint byte inventory

Packed checkpoint magic INFS2026 at 0; format major/minor 8/10, header size 12, shift 14, checksum type 16 (32 bits), generation 20 (64), total blocks 28, free blocks 36, allocation root 44, allocation leaf count 52, object index 60, root object block 68. Three checkpoint positions start 76; UUID 100, root object ID 116; compatible, read-only-compatible and incompatible flags at 132,140,148; label 156 (64 bytes), checksum field 220 (32). Packed header occupies 252 bytes. Validate exact format, nonzero identities and canonical unused bytes; CRC64-ECMA alone does not establish graph validity.

## Recovery locator and discovery

Locator magic INFSL019; schema 8, header size 10, major 12, minor 14, generation hint 16, total blocks 24, three checkpoint positions 32, UUID 56 and CRC64 at 72. Header is 80 bytes; remaining page is zero. Independently validate both locators and primary checkpoint geometry. At most nine map-context checkpoint candidates result. Select the newest structurally complete committed graph of the correct identity; do not conceal I/O or resource errors as corruption. A read-only open never repairs metadata. If all bootstrap records are lost, normal bounded mount cannot infer the volume.

## Allocation-tree layout

Metadata allocation pages have magic INFSAL01 (leaf) or INFSAB01 (branch), generation 8, logical index 16, level 24, entry count 28, used bytes 32, checksum type 36, 32-byte checksum field 40. Header 72; payload 4024. One leaf covers 32192 allocation bits. One branch holds 503 64-bit child addresses. Level-3 root is fixed. Block 35000 belongs to leaf 1 with local bit 2808. Traverse validated branch paths; reserve tail padding, checkpoint/locator regions and metadata. Zero means free, one allocated/unavailable. Runtime free extents are rebuildable, not an independent ownership authority.

## Persistent objects and metadata pages

Object header INFOBJ01 at 0, type 8, version 10, header size 12, generation 16, object ID 24, parent ID 40, payload size 56, checksum type 60, checksum field 64. Header 96 bytes. Independent metadata pages have magic 0, generation 8, owner ID 16, entry count 32, bytes used 36, checksum type 40, reserved 44, checksum field 48; header 80. Distinguish namespace object identity from physical page address. Object-index lookup resolves stable object IDs to current metadata blocks and type; validate index-to-object identity and generation before following payload references.

## Attributes, namespace and hard links

Common attributes occupy 120 bytes: logical size 0, link count 8, flags 16; timestamps at 24,40,56,72 (signed seconds, nanoseconds, reserved); security ID 88, extension ID 104. POSIX compatibility record follows where specified; it is not the portable security authority. Directory record header 24 bytes: size 0, name length 2, type 4, flags 6, target ID 8; UTF-8 name follows, padded to eight bytes. Names 1..1023 bytes; NUL, slash, dot and dot-dot are not stored components. Default comparison is byte-exact; optional casefold v1 folds ASCII only. Regular-file hard-link counts equal reachable name edges. Directories and symlinks are single-parent.

## File representation and translation

An extent is 24 bytes: logical block 0 (64), physical block 8 (64), logical count 16 (32), flags 20 (32). Kind low two bits: normal 0, hole 1. Codec low bits at 2..3 and extension bits 23..31; stored byte count occupies bits 4..22. Validate feature/profile and current encoding; do not rely on older comments describing the original 28-bit count field. Normal extent maps relative logical block to physical start plus displacement. Example logical 5, physical 100, length 3: logical 6 maps block 101. Holes yield zeros; compressed extents require bounded decode and use ceil(stored bytes/4096) physical blocks. Default storage policy permits inline data up to 3840 bytes; other policies may force extent backing.

## Checksums, compression and sharing

Logical data integrity is SHA-256 over decoded logical data according to the defined checksum contract. Metadata uses CRC64-ECMA and canonical reserved bytes. IAC1 v1 codec ID 2 is the current automatic compression choice; LZ4 ID 1 is nondefault. Compression is selected when it saves blocks. Shared ordinary extents permit reflinks; mutation breaks sharing through CoW. A compressed logical slice is not automatically a physical subrange; materialize or replace the bounded cluster as required. Reconcile metadata ownership, ordinary data sharing and snapshots before reclaim. CRC and unkeyed hashes are integrity mechanisms, not proof against arbitrary malicious whole-volume replacement.

## Transaction publication and resizing

Prepare a new allocation/object graph and flush dependencies before publishing checkpoints. Ordinary commits write/flush each copy separately with generation-rotated first copy. Resize writes locator A after dependencies are durable; then flushes new secondary 1, secondary 2, primary 0, locator B in that order. First durable new checkpoint commits the generation; resize success waits for locator B durability. Preserve old recoverable state before first commit and old primary until secondaries are durable. Publication errors forbid more mutation until close/recovery. A checker must separate structural damage, operational I/O failure and unsupported format.

## Security and platform boundaries

Portable security v2 separates principal IDs, typed platform bindings and shareable descriptors with ordered ACEs. Deterministic descriptor and binding index identities use full-digest validation and bounded collision slots. Windows SID and POSIX authority-plus-ID encodings are not equivalent identities. Portable storage-policy flags use bits 48..55 for replica request and 56..63 for encryption domain. Changing policy on existing content requires a qualified data transformation, not a metadata-only reinterpretation. Adapters map native namespace and security APIs to this model. Status of each feature belongs to ROADMAP.md; qualification belongs to QUALIFICATION.md.

## Independent reader and checker exercise

Implement bounded little-endian field decoding and geometry validation before traversing anything. Discover checkpoint candidates, verify CRC and feature admission, then traverse the allocation root and object index. Resolve the root namespace; validate every directory edge and link count. Reconstruct extent mappings and read holes, ordinary and compressed data under bounds. Compare reachable metadata/data ownership with the allocation tree and retained snapshot state. Separate structural checking from deep data scrub. Verify against known-good independently generated fixtures and inject failures at every checkpoint publication boundary; source availability does not establish successful qualification.

## Proposed 0.20: semantic model only

The proposal introduces storage-set, member, volume, object, allocation and page IDs with explicit scopes. Allocation descriptors bind content and integrity; separate location records bind physical placement. File mappings refer to allocation ID and logical slice rather than physical location. A single typed immutable page/record family, authenticated tree membership, generation publication and lifetime engine replace competing ownership models. The current document ON_DISK_FORMAT.md section 20 is the design authority; candidate revisions are not production compatibility. No claim is made that these mechanisms are installed or qualified.

## 0.20 experimental S0 envelope

The private experimental schema uses 4096-byte pages and a 192-byte header, leaving 3904 payload bytes. Candidate offsets: magic 0, major/minor 8/10, kind/schema 12/14, header 16, required flags 18, payload size 20, set/scope/page IDs 24/40/56, sequence 72, digest 80, member ID 112, block 128, lane 136, physical flags 138, reserved 140, CRC 144, reserved 152, payload 192. These offsets are from src/format20/schema.h at the baseline; the experimental probe kind/magic are explicitly NOT production identifiers. A valid page alone does not prove membership of a committed tree.

## 0.20: lifetime and reconstruction

Direct incoming references, immutable roots, allocation location records and retained state define ownership; a file link count is not a storage reference count. A committed Merkle path binds membership, while physical copy placement is excluded from logical commitment. Surviving leaves without missing internal membership proofs are salvage, not proof of the exact original committed hierarchy. A native quick format can preserve prior allocations only within its stated failure model. Foreign formatting can overwrite or discard all copies; no recovery promise extends to destroyed bytes or missing cryptographic keys. Validate the actual executable candidate profiles separately from these intended invariants.

## Reading map and provenance

Current contract: https://github.com/Infiltrator-Projects/InfiltratorFS/blob/6ec9ff0470fc484dd4c579494bdc6bf3e915dcbb/docs/ON_DISK_FORMAT.md . Packed fields: include/infilfs/format.h and locator.h at that same source. Read ARCHITECTURE.md, COMPRESSION.md, PORTABLE_SECURITY_OBJECTS.md, SECURITY.md and FORMAT-0.19-RECOVERY-DESIGN.md for the complete subsystem contracts. Proposed format: section 20 of ON_DISK_FORMAT.md and experimental src/format20/schema.h. The historical research, qualification and operational documents remain in their original paths. This is an additional engineering reference, not a substitute for the complete normative specification.
