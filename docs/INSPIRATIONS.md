<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Design Inspirations

InfiltratorFS does not copy another filesystem's on-disk format. It treats filesystem history as a catalogue of mechanisms that can be accepted, changed or rejected independently.

| Source | Idea retained or reconsidered |
| --- | --- |
| Amiga OFS/FFS | Compact bitmap-based free-space knowledge; simple explicit on-disk structures. |
| Amiga PFS/SFS | Fast recovery philosophy, transactional thinking and online optimisation ideas. |
| ext4 | Extent-based allocation and practical Linux semantics. |
| XFS | Allocation scalability, extent orientation, allocation-group thinking and online checking direction. |
| NTFS | Rich metadata model, allocation bitmap concept, sparse data and named-metadata ideas. |
| ZFS | End-to-end integrity, distrust of storage, copy-on-write roots, scrub and snapshot philosophy. |
| Btrfs | Reflinks, shared extents, snapshots and Linux-native CoW ideas. |
| APFS | Modern CoW metadata, clone/snapshot concepts and encryption-domain thinking. |
| F2FS | Storage-media awareness and flash-sensitive allocation policy. |
| bcachefs | Modern combination of checksums, compression, replication, snapshots and selectable CoW behaviour. |
| HAMMER/HAMMER2 | Historical/versioned filesystem state and transaction-oriented design. |
| Database engines | Atomic publication, generations, page checksums and rebuildable secondary indexes. |

## Ideas intentionally rejected as fundamentals

- linked cluster chains as used by FAT;
- a fixed global inode table as the permanent identity namespace;
- one irreplaceable superblock;
- trusting a successful read without validating critical metadata;
- forcing every workload through one data-write policy;
- making a performance index the only copy of allocation truth;
- designing recovery only after the normal format is finished.

## New combinations explored by InfiltratorFS

### Authoritative bitmap plus disposable free-extent index

The exact bitmap remains small, simple and reconstructable. A later free-extent tree exists to make large allocations fast, but it can be thrown away and rebuilt.

### Persistent object identity independent of pathname and location

A 128-bit object ID survives rename and physical relocation. Namespace, physical placement and identity are separate concepts.

### Workload-aware data policy

The filesystem may automatically recognise static, sequential-growing, random-write, temporary and VM-like workloads and alter placement, CoW and compression behaviour without changing the application's ordinary file interface.

### Protection classes

Files may eventually request stronger or weaker redundancy while retaining a common filesystem namespace.

### Recovery-oriented redundancy

Small amounts of intentionally redundant metadata may be worthwhile when they make reconstruction possible after the primary index is damaged. Redundant hints do not become conflicting sources of truth; one representation remains authoritative during normal operation.
