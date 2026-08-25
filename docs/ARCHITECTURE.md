<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Architecture

## 1. Identity and scope

InfiltratorFS is a clean-sheet, platform-neutral local filesystem. Linux is the primary mounted development host. Windows currently has native storage and transfer tooling over the same persistent structures but not yet an Explorer filesystem driver.

Implementation 0.17.0 uses on-disk Format 0.12. Pre-1.0 development is current-format-only: a future development format may replace Format 0.12 without a migration requirement.

The design assumes power loss is ordinary, storage can return incorrect data, committed metadata must not depend on one physical root copy, and allocation/integrity policy should remain independent of one operating system.

## 2. Layering

InfiltratorFS has four layers:

1. **On-disk format** — fixed-width little-endian records, persistent identities, allocation, transactions, snapshots and integrity metadata.
2. **Portable core engine** — namespace, inline data, extents, checksums, CoW, recovery and scrub logic written in C17.
3. **Platform storage services** — positioned read/write, durable flush, target size, secure randomness, clock and close callbacks.
4. **Operating-system adapters** — the native Linux VFS driver plus POSIX/Win32 direct-storage tooling.

The persistent core does not depend on Linux VFS inode structures, Windows handles, POSIX file descriptors or another filesystem implementation. Core operations use stable `infs_status` values and fixed-width types at the storage boundary.

The old Linux FUSE adapter was a bring-up/reference implementation. It was removed from the 0.17 source and product paths. Linux mounting now means the native `infiltratorfs.ko` VFS driver.

## 3. Persistent object model

Files, directories, checksum records, snapshots and metadata structures are persistent objects identified by 128-bit IDs. A pathname is a namespace mapping rather than object identity.

Directories map UTF-8 names to object IDs. Renaming or relocating an object does not change its persistent identity. Linux derives stable inode identity from the object ID; Windows tooling can expose the same persistent ID without altering the disk representation.

Format 0.12 uses checksummed paged metadata for directories, object indexes and highly fragmented extent maps. Root/checkpoint bootstrap information remains separately replicated.

## 4. Attributes and namespace

Common attributes contain logical size, link count, portable flags, birth/access/content-modification/metadata-change times and reserved references for future security/xattr objects. Times are signed nanoseconds since the Unix epoch as a disk encoding; adapters translate them to native platform forms.

POSIX mode and numeric UID/GID compatibility metadata are retained separately from the portable security model.

Names are well-formed UTF-8, byte-exact and case-sensitive. NUL and `/` are invalid inside a stored component. `.` and `..` are traversal syntax rather than stored entries.

Format 0.12 supports first-class symbolic links and regular-file hard links. Hard links share one persistent regular-file object and exact reference count. Directories and symbolic links remain single-parent namespace objects.

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

One bitmap bit describes one 4096-byte block. The bitmap is authoritative free-space state.

Regular files may use:

- inline storage for small non-empty files up to the current inline threshold;
- ordinary extents for allocated logical blocks;
- hole extents for sparse zero ranges;
- shared normal extents for reflinks; and
- paged extent metadata for highly fragmented files.

Data allocation and metadata allocation progress from opposite directions to preserve sequential locality and reduce metadata/data collision pressure. Runtime hashed object and directory indexes accelerate repeated metadata lookup without changing the persistent format.

## 7. Integrity and recovery

Format 0.12 uses CRC64-ECMA for checkpoints and metadata and SHA-256 for logical file data. Extent-backed logical blocks have checksum metadata; inline data carries its digest inside the authenticated file metadata object.

Normal reads validate data before returning it. Scrub validates the live graph plus retained snapshot generations. Recovery selects a graph-valid committed checkpoint generation rather than trusting checkpoint checksum validity alone.

The raw forensic scanner independently authenticates recognizable metadata blocks and never modifies the target.

## 8. Snapshots and retained generations

Named snapshots retain immutable generation, root, object-index and allocation-state references. CoW superseded blocks remain allocated while any snapshot owns them. Snapshot deletion reclaims only blocks no longer reachable from either the live graph or another retained generation.

Snapshot browsing is read-only through the portable core/direct-image interface. Native undelete and rollback policy remain future work.

## 9. Native Linux VFS

Linux 0.17 mounts through `infiltratorfs.ko` registered as filesystem type `infiltratorfs`.

The driver currently provides native Format 0.12 lookup/enumeration/read support plus read-write create, mkdir, write, setattr and durability publication paths. It reads inline/extents/sparse/paged data directly from the block device and uses the Format 0.12 transaction/integrity model rather than a userspace mount daemon.

The standard `mount.infiltratorfs` helper invokes util-linux `mount -i -t infiltratorfs`, and InfiltratorFS Manager performs the same native mount through its constrained privileged helper. The package/installer builds and installs the module through DKMS.

Further VFS work remains for broader namespace mutation coverage, page-cache/readahead/mmap integration, performance tuning and wider concurrency/stress qualification.

## 10. Desktop integration

`infilfs-inspect --udev` exposes filesystem type, label, UUID and block-size properties for udev/UDisks discovery. The installed udev rule loads the native module when an InfiltratorFS volume is recognized and provides safe default mount options.

InfiltratorFS Manager can format supported non-system partitions, inspect/scrub/forensically scan them, and mount them through the native kernel filesystem.

Stock GNOME Disks may not list InfiltratorFS in its built-in format dropdown until upstream UDisks/libblockdev gains a formatter entry. That does not prevent already-formatted volumes from being identified and mounted through the installed integration.

## 11. Windows

Windows uses the same portable core and Format 0.12 through Win32 image/raw-partition storage. The current Windows application can discover supported partitions, transfer data and scrub them. A Windows kernel filesystem driver remains future work.

## 12. Non-goals and future directions

InfiltratorFS does not use FAT-style linked allocation, a fixed global inode table, a single irreplaceable superblock, unchecked critical metadata or synchronous global deduplication.

Future work includes scalable generation-aware trees, compression, media/workload-aware placement, protection classes, security objects/ACL mapping, encryption domains, broader native Linux VFS coverage and a native Windows filesystem driver.
