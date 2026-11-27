# InfiltratorFS

## 1. Purpose, architecture and format authority

InfiltratorFS is an independently designed, platform-neutral filesystem. Its portable on-disk format defines files, directories, allocation, transactions, snapshots and integrity. Native operating-system adapters connect that format to the host. The Linux package includes the infiltratorfs.ko kernel driver, filesystem tools and the InfiltratorFS Manager.

This unified manual covers the implemented Format 0.19 in chapters 2–15 and the experimental Format 0.20 design in chapters 16–18. The exact byte contract is ON_DISK_FORMAT.md together with include/infilfs/format.h and locator.h. The candidate 0.20 schema is not a production format. The project remains pre-1.0: use disposable or backed-up media and consult exact-source qualification evidence before relying on any feature.

## 2. Installation and device selection

The Debian package installs the Manager and tools, and supplies the source for the native Linux kernel module. DKMS builds and installs infiltratorfs.ko for the target kernel; matching kernel headers are required. The package also prepares initramfs integration when the host supports it.

The hardware-native installer obtains the exact released source and builds on the target machine. The released Debian package can be used when an offline package is required. Release information and package assets are maintained on the project's GitHub Releases page.

The Linux Manager provides a graphical interface for working with selected non-system partitions. Check the device and partition identity carefully before any operation. Formatting removes the existing data on the selected partition; maintain a separate verified backup when the data matters.

On Linux Mint, the project's Nemo Format action is designed for an existing partition. The system's whole-device Mintstick formatter has different behaviour. Follow the on-screen device and partition information rather than assuming the two actions are interchangeable.

## 3. Mounting a volume

The Linux driver can be loaded and a suitable volume mounted with the native mount helper. Replace the example device and mount point with the correct values for the target system.

    sudo modprobe infiltratorfs
    sudo mount -t infiltratorfs -o rw /dev/<partition> /mnt/infiltratorfs
    findmnt -T /mnt/infiltratorfs -o SOURCE,FSTYPE,OPTIONS

The reported filesystem type should be infiltratorfs.

## 4. Format 0.19: geometry and identity

Blocks are 4096 bytes and integers are little endian. Object and filesystem identities are nonzero 128-bit values. Minimum media is 4096 blocks (16 MiB). Initial checkpoints are blocks 0, floor(N/2), N-1. Fixed locators at 1 and 2047 remain reserved during resize and throughout retained generations. Committed capacity may differ from physical capacity. Example: 16384 committed blocks occupy 64 MiB, with checkpoints at 0,8192,16383. Never recalculate checkpoint placement from a larger physical backing device.

## 5. Checkpoint byte inventory

Packed checkpoint magic INFS2026 at 0; format major/minor 8/10, header size 12, shift 14, checksum type 16 (32 bits), generation 20 (64), total blocks 28, free blocks 36, allocation root 44, allocation leaf count 52, object index 60, root object block 68. Three checkpoint positions start 76; UUID 100, root object ID 116; compatible, read-only-compatible and incompatible flags at 132,140,148; label 156 (64 bytes), checksum field 220 (32). Packed header occupies 252 bytes. Validate exact format, nonzero identities and canonical unused bytes; CRC64-ECMA alone does not establish graph validity.

## 6. Recovery locator and discovery

Locator magic INFSL019; schema 8, header size 10, major 12, minor 14, generation hint 16, total blocks 24, three checkpoint positions 32, UUID 56 and CRC64 at 72. Header is 80 bytes; remaining page is zero. Independently validate both locators and primary checkpoint geometry. At most nine map-context checkpoint candidates result. Select the newest structurally complete committed graph of the correct identity; do not conceal I/O or resource errors as corruption. A read-only open never repairs metadata. If all bootstrap records are lost, normal bounded mount cannot infer the volume.

## 7. Allocation-tree layout

Metadata allocation pages have magic INFSAL01 (leaf) or INFSAB01 (branch), generation 8, logical index 16, level 24, entry count 28, used bytes 32, checksum type 36, 32-byte checksum field 40. Header 72; payload 4024. One leaf covers 32192 allocation bits. One branch holds 503 64-bit child addresses. Level-3 root is fixed. Block 35000 belongs to leaf 1 with local bit 2808. Traverse validated branch paths; reserve tail padding, checkpoint/locator regions and metadata. Zero means free, one allocated/unavailable. Runtime free extents are rebuildable, not an independent ownership authority.

## 8. Persistent objects and metadata pages

Object header INFOBJ01 at 0, type 8, version 10, header size 12, generation 16, object ID 24, parent ID 40, payload size 56, checksum type 60, checksum field 64. Header 96 bytes. Independent metadata pages have magic 0, generation 8, owner ID 16, entry count 32, bytes used 36, checksum type 40, reserved 44, checksum field 48; header 80. Distinguish namespace object identity from physical page address. Object-index lookup resolves stable object IDs to current metadata blocks and type; validate index-to-object identity and generation before following payload references.

## 9. Attributes, namespace and hard links

Common attributes occupy 120 bytes: logical size 0, link count 8, flags 16; timestamps at 24,40,56,72 (signed seconds, nanoseconds, reserved); security ID 88, extension ID 104. POSIX compatibility record follows where specified; it is not the portable security authority. Directory record header 24 bytes: size 0, name length 2, type 4, flags 6, target ID 8; UTF-8 name follows, padded to eight bytes. Names 1..1023 bytes; NUL, slash, dot and dot-dot are not stored components. Default comparison is byte-exact; optional casefold v1 folds ASCII only. Regular-file hard-link counts equal reachable name edges. Directories and symlinks are single-parent.

## 10. File representation and translation

An extent is 24 bytes: logical block 0 (64), physical block 8 (64), logical count 16 (32), flags 20 (32). Kind low two bits: normal 0, hole 1. Codec low bits at 2..3 and extension bits 23..31; stored byte count occupies bits 4..22. Validate feature/profile and current encoding; do not rely on older comments describing the original 28-bit count field. Normal extent maps relative logical block to physical start plus displacement. Example logical 5, physical 100, length 3: logical 6 maps block 101. Holes yield zeros; compressed extents require bounded decode and use ceil(stored bytes/4096) physical blocks. Default storage policy permits inline data up to 3840 bytes; other policies may force extent backing.

## 11. Checksums, compression and sharing

Logical data integrity is SHA-256 over decoded logical data according to the defined checksum contract. Metadata uses CRC64-ECMA and canonical reserved bytes. IAC1 v1 codec ID 2 is the current automatic compression choice; LZ4 ID 1 is nondefault. Compression is selected when it saves blocks. Shared ordinary extents permit reflinks; mutation breaks sharing through CoW. A compressed logical slice is not automatically a physical subrange; materialize or replace the bounded cluster as required. Reconcile metadata ownership, ordinary data sharing and snapshots before reclaim. CRC and unkeyed hashes are integrity mechanisms, not proof against arbitrary malicious whole-volume replacement.

## 12. Transaction publication and resizing

Prepare a new allocation/object graph and flush dependencies before publishing checkpoints. Ordinary commits write/flush each copy separately with generation-rotated first copy. Resize writes locator A after dependencies are durable; then flushes new secondary 1, secondary 2, primary 0, locator B in that order. First durable new checkpoint commits the generation; resize success waits for locator B durability. Preserve old recoverable state before first commit and old primary until secondaries are durable. Publication errors forbid more mutation until close/recovery. A checker must separate structural damage, operational I/O failure and unsupported format.

## 13. Checking, scrubbing and maintenance

The normal fsck.infiltratorfs check is structural: it checks checkpoints, allocation, objects, namespaces and checksum metadata. It does not read all file data or recompute every payload checksum.

The optional --scrub mode checks data integrity more deeply and may take much longer. It is a mode of fsck.infiltratorfs, not a separate executable. FSCK-SCRUB-SEPARATION.md defines these semantics.

The filesystem supports retained generations and named read-only snapshots. Linux tools include fragmentation metrics, bounded online defragmentation, online grow and bounded shrink. Native user, group and project quota rules include durable limits and reconstruction of usage on remount; multiple names for one inode must not double-charge that object. Consult ROADMAP.md and QUALIFICATION.md for the exact prerequisites and scope of these capabilities.

Snapshots are not a substitute for a separate backup. Qualification applies to particular source revisions and evidence classes; older tests do not prove every later release or physical-media scenario.

## 14. Platform support and development compatibility

Linux is the most complete mounted adapter. Windows includes image/raw-partition tooling and a driverless Explorer projection. A native Windows driver exists in source, but mounted Windows and related qualification remain outstanding.

Development-format compatibility before version 1.0 is not promised. Test media may require reformatting after an on-disk-format change. The experimental Common V2 module has hosted and freestanding integration paths. Its existence does not establish a production BOOT reader: the generic Common V2 loader and exact-source boot qualification remain separate requirements.

Portable security v2 separates principal IDs, typed platform bindings and shareable descriptors with ordered ACEs. Deterministic descriptor and binding index identities use full-digest validation and bounded collision slots. Windows SID and POSIX authority-plus-ID encodings are not equivalent identities. Portable storage-policy flags use bits 48..55 for replica request and 56..63 for encryption domain. Changing policy on existing content requires a qualified data transformation, not a metadata-only reinterpretation. Adapters map native namespace and security APIs to this model. Status of each feature belongs to ROADMAP.md; qualification belongs to QUALIFICATION.md.

## 15. Independent reader and checker exercise

Implement bounded little-endian field decoding and geometry validation before traversing anything. Discover checkpoint candidates, verify CRC and feature admission, then traverse the allocation root and object index. Resolve the root namespace; validate every directory edge and link count. Reconstruct extent mappings and read holes, ordinary and compressed data under bounds. Compare reachable metadata/data ownership with the allocation tree and retained snapshot state. Separate structural checking from deep data scrub. Verify against known-good independently generated fixtures and inject failures at every checkpoint publication boundary; source availability does not establish successful qualification.

## 16. Proposed Format 0.20 — semantic model (not production)

The proposal introduces storage-set, member, volume, object, allocation and page IDs with explicit scopes. Allocation descriptors bind content and integrity; separate location records bind physical placement. File mappings refer to allocation ID and logical slice rather than physical location. A single typed immutable page/record family, authenticated tree membership, generation publication and lifetime engine replace competing ownership models. The current document ON_DISK_FORMAT.md section 20 is the design authority; candidate revisions are not production compatibility. No claim is made that these mechanisms are installed or qualified.

## 17. Proposed 0.20 — experimental S0 envelope

The private experimental schema uses 4096-byte pages and a 192-byte header, leaving 3904 payload bytes. Candidate offsets: magic 0, major/minor 8/10, kind/schema 12/14, header 16, required flags 18, payload size 20, set/scope/page IDs 24/40/56, sequence 72, digest 80, member ID 112, block 128, lane 136, physical flags 138, reserved 140, CRC 144, reserved 152, payload 192. These teaching offsets are from src/format20/schema.h at the historical 6ec9ff0470fc484dd4c579494bdc6bf3e915dcbb baseline; the experimental probe kind/magic are explicitly NOT production identifiers. A valid page alone does not prove membership of a committed tree.

## 18. Proposed 0.20 — lifetime and reconstruction

Direct incoming references, immutable roots, allocation location records and retained state define ownership; a file link count is not a storage reference count. A committed Merkle path binds membership, while physical copy placement is excluded from logical commitment. Surviving leaves without missing internal membership proofs are salvage, not proof of the exact original committed hierarchy. A native quick format can preserve prior allocations only within its stated failure model. Foreign formatting can overwrite or discard all copies; no recovery promise extends to destroyed bytes or missing cryptographic keys. Experimental results are recorded separately from the design. At ce9dce04e9d3dabcd9243f5198d0f103c4858441, eleven dedicated tests passed, including a non-installed read-only image exporter with named output and verified partial fragments. At c4bc7dba6f40362a81f6a92673448943cd5e0d6d, seventeen tests passed, including foreign formatter characterization. The exFAT experiment destroyed the sample payloads despite discoverable committed metadata; correct refusal to export was a test pass, not successful recovery. At 7b0f572d59fa878225476c9251e55f1e35947a6c, twenty-five dedicated experimental tests passed. These results do not qualify the production format, native mounting, physical media or BOOT. See QUALIFICATION.md for evidence and limits.

## 19. Installed files, sources and reading map

The Manager and tools are provided by the InfiltratorFS package. The Linux kernel module is installed through DKMS. This guide is installed as /System/Resources/Documentation/InfiltratorFS.pdf.

The project README gives the product overview. ON_DISK_FORMAT.md, ROADMAP.md and QUALIFICATION.md govern the format, feature status and qualification evidence.

Project: https://github.com/Infiltrator-Projects/InfiltratorFS

Current normative contract: https://github.com/Infiltrator-Projects/InfiltratorFS/blob/main/docs/ON_DISK_FORMAT.md . Packed fields: include/infilfs/format.h and locator.h at the same source as the software being examined. The byte-layout teaching inventory in chapters 4–12 and experimental envelope in chapter 17 were derived from the historical 6ec9ff0470fc484dd4c579494bdc6bf3e915dcbb baseline; consult the exact-source headers for authoritative fields. Read ARCHITECTURE.md, COMPRESSION.md, PORTABLE_SECURITY_OBJECTS.md, SECURITY.md and FORMAT-0.19-RECOVERY-DESIGN.md for the complete subsystem contracts. Proposed format: section 20 of ON_DISK_FORMAT.md and experimental src/format20/schema.h. The historical research, qualification and operational documents remain in their original paths. This manual does not supersede the complete normative specification.
