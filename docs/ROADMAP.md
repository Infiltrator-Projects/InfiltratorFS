<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Roadmap

Pre-1.0 development is current-format-only. A new development format may supersede the current format without a backward reader or migration path. Backward compatibility begins with the first stable release.

## Completed foundation

- [x] Three physically separated checkpoints and generation-based recovery.
- [x] Authoritative allocation bitmap and copy-on-write transactions.
- [x] 128-bit persistent filesystem/object identities.
- [x] CRC64 metadata integrity and SHA-256 file-data integrity.
- [x] Portable C17 core with callback-based storage services.
- [x] Stable `infs_status` error contract and platform-neutral packed format.
- [x] Exact-format conformance, malformed-image rejection and crash testing.

## Completed Format 0.12 storage features

- [x] UTF-8 namespace and portable/POSIX metadata split.
- [x] Inline small files.
- [x] Sparse files and hole punching.
- [x] Shared extents and reflinks.
- [x] Paged directories, object indexes and fragmented extent metadata.
- [x] Symbolic links and regular-file hard links.
- [x] Named read-only snapshots and retained historical generations.
- [x] Online snapshot-coordinated scrub.
- [x] Raw forensic metadata discovery.
- [x] Operation-level transaction savepoints.
- [x] Runtime hashed metadata indexes.
- [x] Sequential allocation and checksum hot-path performance work.

## Completed Linux 0.17 native filesystem milestone

- [x] Native out-of-tree Linux VFS filesystem driver `infiltratorfs.ko`.
- [x] DKMS packaging and host-kernel rebuild/install path.
- [x] Native Format 0.12 read-only lookup, enumeration and file reads.
- [x] Native read-write create, mkdir, setattr and extent-backed file writes.
- [x] Native transaction publication at durability boundaries.
- [x] Metadata and file-data integrity checking on the native path.
- [x] Native `mount.infiltratorfs` helper using `mount -i -t infiltratorfs`.
- [x] InfiltratorFS Manager native block-device and loop-image mounting.
- [x] udev/UDisks filesystem identification and desktop visibility.
- [x] Native-only Debian and `.run` packaging with no FUSE runtime dependency.
- [x] Remove the legacy FUSE implementation, mounted workflow and FUSE-specific tests from the current source tree.
- [x] Release publication gate that installs the generated `.deb`, mounts `FSTYPE=infiltratorfs`, writes/reads non-zero data, syncs, unmounts and requires a clean scrub.

## Linux next work

- [ ] Complete the remaining VFS namespace mutation surface.
- [ ] Page-cache, readahead and mmap integration.
- [ ] Broader locking/concurrency qualification.
- [ ] Millions-of-files and large-volume mounted stress tests.
- [ ] Native fragmentation/optimisation metrics and online defragmentation.
- [ ] Upstream/libblockdev formatter integration so stock GNOME Disks can offer InfiltratorFS directly in its format menu.

## Scale and performance

- [ ] Scalable generation-aware object-index tree.
- [ ] Scalable directory trees.
- [ ] Rebuildable free-extent index.
- [ ] Parallel allocation model.
- [ ] Locality scoring and workload-aware placement.
- [ ] Media-aware placement.
- [ ] Per-extent compression.

## Security and protection

- [ ] Versioned security objects with typed principals and ACL entries.
- [ ] Linux UID/GID/mode mapping policy over portable security objects.
- [ ] Windows SID/security-descriptor mapping policy.
- [ ] Native extended-attribute and named-data-stream objects.
- [ ] Protection classes and multi-device placement.
- [ ] Replication and/or parity.
- [ ] Encryption domains and key wrapping.
- [ ] Authenticated metadata for encrypted volumes.

## Namespace portability

- [ ] Versioned Unicode normalization policy.
- [ ] Optional case-folded directory policy.
- [ ] Cross-platform removable-volume filename profile.
- [ ] Generic reparse-point objects.

## Windows

- [x] Win32 image/device and bounded raw-partition storage backend.
- [x] Windows storage/partition conformance harness.
- [x] Windows formatter, root transfer/listing and scrub application.
- [x] Shared Linux/Windows Format 0.12 interoperability tests.
- [ ] Windows attribute/security/filename adapter completion.
- [ ] Native Windows filesystem driver and Explorer drive-letter mounting.

## Deliberately deferred

Global synchronous deduplication, distributed/network filesystem semantics and application-visible transactions are not first-generation requirements.
