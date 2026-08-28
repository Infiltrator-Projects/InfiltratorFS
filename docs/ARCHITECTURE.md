<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Architecture

## 1. Identity and scope

InfiltratorFS is a clean-sheet, platform-neutral local filesystem. The filesystem is defined by its on-disk format and portable core semantics, not by any one operating system. Linux is currently the most complete mounted adapter; Windows has native storage/transfer tooling over the same persistent structures but not yet a Windows kernel filesystem driver.

Development 0.18.18 uses on-disk Format 0.15. Pre-1.0 development is current-format-only: a future development format may replace Format 0.15 without a migration requirement.

The design assumes power loss is ordinary, storage can return incorrect data, committed metadata must not depend on one physical root copy, and allocation, integrity, security and namespace policy must remain independent of one operating system.

## 2. Layering

InfiltratorFS has four architectural layers:

1. **On-disk format** — fixed-width little-endian records, persistent identities, allocation, transactions, snapshots and integrity metadata.
2. **Portable core engine** — namespace, inline data, extents, checksums, CoW, recovery and scrub logic written in C17.
3. **Platform storage services** — positioned read/write, durable flush, target size, secure randomness, clock and close callbacks.
4. **Operating-system adapters** — native filesystem drivers and direct-storage tools that translate platform APIs into InfiltratorFS semantics.

The persistent core does not depend on Linux VFS inode structures, Windows handles, POSIX file descriptors or another filesystem implementation. Core operations use stable `infs_status` values and fixed-width types at the storage boundary.

The Linux native driver is one adapter over this model, not the canonical definition of the filesystem. A future Windows, macOS, BSD or Haiku driver should map native semantics to the same core rather than emulate Linux. See `PLATFORM_ADAPTERS.md`.

The old Linux FUSE adapter was a bring-up/reference implementation. It was removed from the current source and product paths. Linux mounting now means the native `infiltratorfs.ko` VFS driver.

## 3. Persistent object model

Files, directories, checksum records, snapshots and metadata structures are persistent objects identified by 128-bit IDs. A pathname is a namespace mapping rather than object identity.

Directories map UTF-8 names to object IDs. Renaming or relocating an object does not change its persistent identity. Linux derives stable inode identity from the object ID; other adapters can expose the same persistent identity without altering the disk representation.

Format 0.15 uses a generation-aware object-index radix tree, hashed directory trees and checksummed paged metadata for highly fragmented extent maps. Root/checkpoint bootstrap information remains separately replicated.

## 4. Attributes, namespace and adapter metadata

Common attributes contain logical size, link count, portable flags, birth/access/content-modification/metadata-change times and reserved references for future portable security and named-metadata objects. Times are signed nanoseconds since the Unix epoch as a disk encoding; adapters translate them to native platform forms.

POSIX mode and numeric UID/GID compatibility metadata are retained separately from the planned portable security model. Development 0.18.18 also persists standard Linux xattr namespaces and special-node metadata through Linux adapter metadata; this does not make Linux metadata the cross-platform canonical representation. The reserved generic extended-attribute/security object references remain available for the future portable model.

Names are well-formed UTF-8, byte-exact and case-sensitive in Format 0.15. NUL and `/` are invalid inside a stored component. `.` and `..` are traversal syntax rather than stored entries. Future adapters may need policy layers such as case folding or normalization without changing object identity.

Format 0.15 supports first-class symbolic links and regular-file hard links. Hard links share one persistent regular-file object and exact reference count. Directories and symbolic links remain single-parent namespace objects.

## 5. Transaction model

Committed critical metadata is never overwritten as its only valid copy. A transaction based on generation `N`:

1. snapshots committed allocation state;
2. reserves replacement blocks;
3. writes new data and metadata to unreachable blocks;
4. updates the object/namespace graph through copy-on-write;
5. constructs the authoritative replacement allocation state;
6. issues the required durable flushes;
7. publishes a generation `N+1` checkpoint as the atomic commit point;
8. replicates that checkpoint to the other physical checkpoint locations; and
9. reclaims superseded blocks only when no live or retained snapshot generation still owns them.

A crash before publication retains generation `N`; a durable first checkpoint exposes generation `N+1`. Read-only recovery may use surviving replicas. Writable recovery fails closed when it cannot safely establish the newest durable generation.

Operation-level savepoints prevent one failed mutation from discarding earlier acknowledged buffered changes.

## 6. Allocation and file representations

One bitmap bit describes one 4096-byte block. The bitmap is authoritative free-space state. Portable and native Linux writers rebuild an in-memory index of maximal free extents from that bitmap and maintain it during allocation. The index is an accelerator only: it is never persisted, may be discarded under memory pressure or rollback, and allocator correctness falls back to the authoritative bitmap scan if the cache cannot satisfy a request.

Regular files may use:

- inline storage for small non-empty files up to the current inline threshold;
- ordinary extents for allocated logical blocks;
- hole extents for sparse zero ranges;
- shared normal extents for reflinks; and
- paged extent metadata for highly fragmented files.

The current native Linux path supports sequential and random extent writes, sparse growth, high-offset writes, truncate, `fallocate`, hole punching and allocation reporting. Shared extents are broken through CoW when modified.

Data allocation and metadata allocation progress from opposite directions to preserve sequential locality and reduce metadata/data collision pressure. Their rebuildable free-extent indexes preserve those cursor preferences while reconsidering complete extents when a cursor bisects a free run, preventing false out-of-space results. Runtime hashed object and directory indexes accelerate repeated metadata lookup without changing the persistent format.

## 7. Integrity and recovery

Format 0.15 uses CRC64-ECMA for checkpoints and metadata and SHA-256 for logical file data. Extent-backed logical blocks have checksum metadata; inline data carries its digest inside the authenticated file metadata object.

Normal reads validate data before returning it. Scrub validates the live graph plus retained snapshot generations. Recovery selects a graph-valid committed checkpoint generation rather than trusting checkpoint checksum validity alone.

The raw forensic scanner independently authenticates recognizable metadata blocks and never modifies the target.

## 8. Snapshots and retained generations

Named snapshots retain immutable generation, root, object-index and allocation-state references. CoW superseded blocks remain allocated while any snapshot owns them. Snapshot deletion reclaims only blocks no longer reachable from either the live graph or another retained generation.

Development 0.18.18 qualifies writable live namespace/data changes while retained snapshots continue to expose the older generation. Snapshot browsing remains read-only through the portable core/direct-image interface. Native undelete and rollback policy remain future work.

## 9. Native Linux VFS

Linux development 0.18.18 mounts through `infiltratorfs.ko` registered as filesystem type `infiltratorfs`.

The driver provides native Format 0.15 lookup/enumeration/read support and a broad read-write surface: create/mkdir/mknod, link/symlink, rename/unlink/rmdir, persistent setattr, regular and sparse writes, truncate, `fallocate`, hole punching, hard-link/open-unlink lifetime, mount-time orphan recovery, standard Linux xattr namespaces, FIFO/socket/character/block node identity, page-cache faults, readahead and shared writable `mmap` writeback. It preserves snapshot-owned historical blocks while the live namespace changes.

The driver reads inline/extents/sparse/paged data directly from the block device and uses the Format 0.15 transaction/integrity model rather than a userspace mount daemon. Native reads verify metadata and file-data integrity; durability publication occurs through `fsync`, `syncfs` and global sync paths.

The standard `mount.infiltratorfs` helper invokes util-linux `mount -i -t infiltratorfs`, and InfiltratorFS Manager performs the same native mount through its constrained privileged helper. The package/installer builds and installs the module through DKMS.

Further Linux work is primarily wider recovery, locking/concurrency, scale, stress and performance qualification rather than restoration of the old FUSE-era feature surface.

## 10. Desktop integration

`infilfs-inspect --udev` exposes filesystem type, label, UUID and block-size properties for udev/UDisks discovery. The installed udev rule loads the native module when an InfiltratorFS volume is recognized and provides safe default mount options.

InfiltratorFS Manager can format supported non-system partitions, inspect/scrub/forensically scan them, and mount them through the native kernel filesystem.

Stock GNOME Disks may not list InfiltratorFS in its built-in format dropdown until upstream UDisks/libblockdev gains a formatter entry. That does not prevent already-formatted volumes from being identified and mounted through the installed integration.

## 11. Windows and additional operating systems

Windows uses the same portable core and Format 0.15 through Win32 image/raw-partition storage. The current Windows application can discover supported partitions, transfer data and scrub them. A Windows kernel filesystem driver remains future work.

The intended design is equal first-class native adapters over the same filesystem model. Linux VFS concepts, Windows I/O/Cache Manager concepts, macOS VFS concepts, BSD vnode semantics and Haiku filesystem APIs are adapter responsibilities. Where concepts are equivalent, they map to one portable InfiltratorFS meaning; where an OS has additional semantics, that metadata is preserved without becoming mandatory for unrelated platforms.

## 12. Security architecture

The long-term security model is independent of POSIX UID/GID and Windows SID representations. Portable security objects will identify stable principals and ACL entries; adapters will map native identities and access masks to those portable principals and rights. Platform-specific security information that cannot be expressed elsewhere must be preserved rather than discarded by another adapter.

Development 0.18.18 does not yet implement the final portable security-object format. Current POSIX compatibility metadata is therefore compatibility state, not the canonical cross-platform identity model. See `SECURITY.md`.

## 13. Non-goals and future directions

InfiltratorFS does not use FAT-style linked allocation, a fixed global inode table, a single irreplaceable superblock, unchecked critical metadata or synchronous global deduplication.

Future work includes rebuildable free-extent indexing, compression, media/workload-aware placement, protection classes, portable security objects/ACL mapping, generic named metadata/streams, encryption domains, broader mounted stress and additional native operating-system drivers.
