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

## Completed native Linux filesystem surface through 0.18.4

- [x] Native out-of-tree Linux VFS filesystem driver `infiltratorfs.ko`.
- [x] DKMS packaging and host-kernel rebuild/install path.
- [x] Native Format 0.12 read-only lookup, enumeration and file reads.
- [x] Native read-write create, mkdir, mknod, setattr and extent-backed writes.
- [x] Link/symlink, rename/unlink/rmdir and persistent metadata changes.
- [x] Random writes, sparse growth, high-offset writes, truncate and `fallocate`.
- [x] Hole punching and logical/allocated block reporting.
- [x] Crash-safe open-unlink/replacement lifetime and mount-time orphan recovery.
- [x] Persistent Linux `user.*` xattrs and FIFO/socket/character/block node identity.
- [x] Page-cache, readahead and shared writable `mmap` integration.
- [x] Writable live generations while named snapshots retain older generations.
- [x] Native transaction publication at durability boundaries.
- [x] Metadata and file-data integrity checking on the native path.
- [x] Native `mount.infiltratorfs` helper using `mount -i -t infiltratorfs`.
- [x] InfiltratorFS Manager native block-device and loop-image mounting.
- [x] udev/UDisks filesystem identification and desktop visibility.
- [x] Native-only Debian and `.run` packaging with no FUSE runtime dependency.
- [x] Removal of the legacy FUSE implementation from the current source/product path.
- [x] Mounted remount/scrub qualification for the migrated native surface.
- [x] Release gate that installs the generated `.deb`, mounts `FSTYPE=infiltratorfs`, writes/reads non-zero data, syncs, unmounts and requires a clean scrub.

## Linux next work

- [ ] Broaden dirty-checkpoint and interrupted-publication recovery qualification on mounted images.
- [ ] Broaden xattr semantics and error-path qualification beyond the current Linux `user.*` surface.
- [ ] Complete directory/symlink/special-object allocation-reporting qualification.
- [ ] Preserve explicit regression coverage for every formerly mounted semantic path.
- [ ] Broader locking/concurrency qualification.
- [ ] Millions-of-files and large-volume mounted stress tests.
- [ ] Wider near-full, fragmentation and long-running mixed-workload tests.
- [ ] Native fragmentation/optimisation metrics and online defragmentation.
- [ ] Upstream/libblockdev formatter integration so stock GNOME Disks can offer InfiltratorFS directly in its format menu.

The first four items above describe active follow-on qualification work and must not be treated as complete until the corresponding code and tests are present on `main`.

## Scale and performance

- [ ] Scalable generation-aware object-index tree.
- [ ] Scalable directory trees.
- [ ] Rebuildable free-extent index.
- [ ] Parallel allocation model.
- [ ] Locality scoring and workload-aware placement.
- [ ] Media-aware placement.
- [ ] Per-extent compression.

## Security and protection

- [ ] Versioned portable security objects with stable typed principals and ACL entries.
- [ ] Generic access-right vocabulary independent of POSIX and Windows constants.
- [ ] Linux UID/GID/mode and POSIX ACL mapping policy.
- [ ] Windows SID/security-descriptor mapping policy.
- [ ] Preservation rules for platform-specific security metadata not understood by another adapter.
- [ ] Portable named-attribute/named-stream objects distinct from adapter sidecars.
- [ ] Protection classes and multi-device placement.
- [ ] Replication and/or parity.
- [ ] Encryption domains and key wrapping.
- [ ] Authenticated metadata for encrypted volumes.

See `SECURITY.md` for the intended cross-platform model. Current POSIX compatibility metadata and Linux xattr sidecars are not the final portable security representation.

## Namespace portability

- [ ] Versioned Unicode normalization policy.
- [ ] Optional case-folded directory policy.
- [ ] Cross-platform removable-volume filename profile.
- [ ] Generic reparse/typed-extension objects.
- [ ] Generic named data streams/extended metadata model.

## Operating-system adapters

- [x] Native Linux VFS adapter is the current mounted reference implementation.
- [x] Win32 image/device and bounded raw-partition storage backend.
- [x] Windows storage/partition conformance harness.
- [x] Windows formatter, root transfer/listing and scrub application.
- [x] Shared Linux/Windows Format 0.12 interoperability tests.
- [ ] Windows attribute/security/filename adapter completion.
- [ ] Native Windows filesystem driver and Explorer drive-letter mounting.
- [ ] macOS native adapter investigation/implementation.
- [ ] BSD native adapter investigation/implementation.
- [ ] Haiku native adapter investigation/implementation.

The adapter architecture is deliberately generic: no future operating system should require redefining the persistent filesystem merely because it uses different API names for equivalent concepts. See `PLATFORM_ADAPTERS.md`.

## Deliberately deferred

Global synchronous deduplication, distributed/network filesystem semantics and application-visible transactions are not first-generation requirements.
