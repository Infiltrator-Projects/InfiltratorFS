<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Design Inspirations

This file records design influences only. It is not a feature-status or qualification document.

InfiltratorFS does not copy another filesystem's on-disk format. It treats filesystem history as a catalogue of mechanisms that can be accepted, changed, generalized or rejected independently.

| Source | Idea retained or reconsidered |
| --- | --- |
| Amiga OFS/FFS | Bitmap-style allocation truth and simple explicit on-disk structures. |
| Amiga PFS/SFS | Fast recovery philosophy, transactional thinking and online optimisation ideas. |
| ext4 | Extent-based allocation and practical Linux semantics. |
| XFS | Allocation scalability, extent orientation and online checking direction. |
| NTFS | Rich metadata model, sparse data and named-metadata ideas. |
| ReFS | Windows-native integrity, allocation-on-write and large-scale metadata ideas. |
| ZFS | End-to-end integrity, distrust of storage, copy-on-write roots, scrub and snapshot philosophy. |
| Btrfs | Reflinks, shared extents, snapshots and Linux-native CoW ideas. |
| APFS | Modern CoW metadata, clone/snapshot concepts and encryption-domain thinking. |
| F2FS | Storage-media awareness and flash-sensitive allocation policy. |
| bcachefs | Checksums, compression, replication, snapshots and selectable CoW behaviour. |
| HAMMER/HAMMER2 | Historical/versioned filesystem state and transaction-oriented design. |
| Haiku BFS | Rich named attributes, indexing/query ideas and metadata-centric desktop integration. |
| Database engines | Atomic publication, generations, page checksums and rebuildable secondary indexes. |

## Ideas intentionally rejected as fundamentals

- linked cluster chains as used by FAT;
- a fixed global inode table as the permanent identity namespace;
- one irreplaceable superblock;
- trusting a successful read without validating critical metadata;
- forcing every workload through one data-write policy;
- making a performance index the only copy of allocation truth;
- designing recovery only after the normal format is finished; and
- making one operating system's metadata vocabulary the filesystem's canonical model.

## Combinations explored by InfiltratorFS

### Bitmap allocation semantics plus rebuildable free-extent acceleration

Allocation ownership remains the simple one-bit-per-block logical model, while Format 0.17 persists that bitset through a scalable sharded copy-on-write allocation tree. Runtime free-extent indexes are disposable accelerators that can be rebuilt from the authoritative allocation state.

### Persistent object identity independent of pathname and location

A 128-bit object ID survives rename and physical relocation. Namespace, physical placement and identity are separate concepts.

### Platform-neutral core with first-class adapters

The disk format and portable core use fixed-width records, UTF-8 names and storage callbacks. Equivalent Linux, Windows, macOS, BSD, Haiku or future filesystem concepts map to the same underlying InfiltratorFS meaning rather than forcing one platform to emulate another.

POSIX compatibility metadata and Linux adapter sidecars are isolated from the long-term portable security/named-metadata model. Future Windows SIDs/security descriptors, macOS metadata, Haiku attributes and other native semantics can be translated or preserved without changing persistent object identity.

### Workload-aware data policy

Placement, CoW and compression policy may respond to workload and media characteristics without changing the application's ordinary file interface or the persistent identity model.

### Protection classes

Files may eventually request stronger or weaker redundancy while retaining a common filesystem namespace.

### Recovery-oriented redundancy

Small amounts of intentional metadata redundancy are worthwhile when they make reconstruction possible after primary metadata damage. Redundant hints do not become conflicting normal-operation sources of truth; committed checkpoint/graph rules remain authoritative.