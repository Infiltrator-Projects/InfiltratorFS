<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Format 0.15 Conformance

Release 0.18.18 accepts exactly current on-disk Format 0.15. Pre-1.0 builds do not promise compatibility with earlier development formats.

## Persistent representation

A conforming implementation preserves:

- 4096-byte little-endian filesystem blocks;
- exact Format 0.15 major/minor identity;
- the packed structure sizes and offsets declared by the public format headers;
- three physical checkpoint locations;
- CRC64-ECMA protection for checkpoints and metadata;
- SHA-256 protection for logical file data;
- nonzero 128-bit filesystem and object identities;
- nonzero committed generations;
- valid UTF-8 labels and namespace components with canonical reserved/padding bytes;
- case-sensitive byte-exact namespace comparison;
- authoritative bitmap ownership for every committed block;
- exact root/object-index/directory/checksum graph reachability;
- exact link counts and parent/reference rules;
- ordinary extents, sparse hole extents and shared normal extents;
- inline small-file representation;
- generation-aware object-index and directory trees plus paged extent metadata;
- symbolic-link objects;
- regular-file hard links;
- snapshot-catalog objects and retained historical generations; and
- common portable attributes plus isolated POSIX compatibility metadata.

Unknown incompatible feature bits are rejected. Unknown read-only-compatible bits may be accepted only read-only. Newly created Format 0.15 volumes enable the currently required known feature set.

## Namespace and object rules

Stored names are well-formed UTF-8. NUL and `/` are forbidden within a component and `.`/`..` are traversal syntax rather than stored entries.

Directories and symbolic links have one namespace parent. Regular files may have multiple directory references when hard links are enabled. A regular file's stored link count must equal its exact reachable reference count.

Symbolic-link targets are nonempty opaque UTF-8 payloads within the current inline target bound. Relative/absolute traversal semantics belong to the operating-system adapter.

## File representations

A regular file may be empty, inline, extent-backed, sparse or reflinked.

Inline non-empty files up to the current inline threshold store their payload and SHA-256 digest inside the file metadata object and own no separate data/checksum blocks.

Extent-backed files have complete logical coverage through ordinary and hole extents. Hole ranges map to zeros without owning data blocks. Every allocated logical data block has SHA-256 checksum coverage through the checksum metadata chain.

Shared normal extents are permitted only when the shared-extents feature is enabled. Metadata ownership may never overlap. Writes to shared data break sharing through copy-on-write.

Highly fragmented files may promote from compact inline extent descriptors to checksummed paged extent metadata. Scrub and ownership validation include those pages.

## Transactions and recovery

Critical committed metadata is copy-on-write. A mutation publishes a new generation only after its replacement graph and required durability operations are complete.

A crash before first-checkpoint publication leaves the previous committed generation authoritative. Once the first new checkpoint is durably published, the new generation is committed even if later replica refresh fails.

Writable recovery considers checkpoint candidates in descending generation order and accepts only a complete graph-valid candidate. Structural corruption may justify falling back to an older committed generation; external I/O, unsupported-format, memory or other operational failures are not hidden as corruption.

If durability of the first checkpoint publication cannot be established, further mutation is disabled until close/reopen recovery.

Operation-level savepoints preserve earlier acknowledged buffered mutations when a later operation fails.

## Snapshots

Each named snapshot identifies an immutable earlier generation, root, object index and retained allocation image. Superseded blocks remain unavailable while any retained generation references them. Deleting a snapshot reclaims only blocks absent from both the live graph and every remaining snapshot graph.

Snapshot lookup, stat, enumeration, read and readlink operations are read-only. Live writes must not mutate or reclaim blocks still owned by a retained snapshot generation.

## Scrub and forensic behavior

`infilfs-scrub` verifies the live graph and retained snapshot data, including ownership, namespace structure, metadata CRC64, inline digests, extent checksums and snapshot retention.

The forensic scanner reads complete physical blocks, independently authenticates recognizable metadata and never modifies the target. Discovery is not treated as authoritative recovery state by itself.

## Portable result contract

The core/storage boundary uses stable `infs_status` values. Byte-count APIs return a non-negative count on success or a negative `infs_status` on failure. Platform adapters translate to native error conventions only at the boundary.

Writable storage backends provide positioned write, durable flush, randomness and current-time services in addition to read/size operations. Required service failures abort mutation rather than substituting unsafe defaults.

## Linux native conformance

Linux mounting is exclusively through the native `infiltratorfs.ko` VFS driver. The current source tree contains no FUSE filesystem implementation and release packages have no FUSE dependency.

Native Linux qualification requires:

- module compilation against supported kernel headers;
- successful DKMS-style source-root build;
- registration of filesystem type `infiltratorfs`;
- native read-only and read-write mount behavior;
- correct lookup/enumeration of current Format 0.15 directories and object indexes;
- inline, ordinary-extent, sparse-hole and paged-extent reads;
- stable inode identity for persistent objects/hard links;
- create/mkdir/mknod, link/symlink, rename/unlink/rmdir and persistent setattr;
- sequential and random extent writes, large sparse growth/truncate, `fallocate` and hole punching;
- correct logical versus allocated-size reporting for ordinary sparse and metadata-bearing objects;
- persistent standard Linux xattr namespaces (`user.*`, `trusted.*`,
  `security.*`, `system.*`) and FIFO/socket/character/block node identity;
- verified page-cache faults, readahead and shared writable `mmap` writeback;
- writable live namespace/data changes while retained snapshots preserve older generations;
- crash-safe open-unlink/replacement lifetime, mount-time orphan recovery and
  correct directory/symlink allocated-block reporting;
- extent-backed non-zero writes and byte-identical live/remount readback;
- metadata CRC64 and file-data SHA-256 validation on native reads;
- transaction publication through `fsync`/`syncfs`/sync durability boundaries;
- clean unmount followed by userspace scrub; and
- refusal to silently substitute a non-native filesystem path.

Release 0.18.18 qualifies this native migrated surface. Additional development qualification is expected to expand locking/concurrency, scale and stress coverage rather than reintroduce a FUSE runtime path.

`mount.infiltratorfs` and InfiltratorFS Manager must produce `FSTYPE=infiltratorfs`. The Manager privileged helper rejects a mounted result with any other filesystem type.

## Adapter conformance

Operating-system adapters may expose different native APIs, but they must not redefine the persistent meaning of shared filesystem concepts. Equivalent native concepts map to the same portable object/extent/transaction semantics. Adapter-only metadata must remain isolated and must not be silently discarded merely because another adapter cannot expose it.

POSIX UID/GID/mode compatibility metadata is not the final cross-platform security authority. The future security-object model is defined architecturally in `SECURITY.md`; until that format exists, adapters must not claim portable ACL/SID equivalence that is not actually stored.

## Desktop/package conformance

The Linux package must contain the formatter, inspector, direct-image tool, scrubber, forensic scanner, Manager, constrained helper, standard mount/fsck helpers, udev rule and self-contained DKMS source.

The package must depend on the native build/runtime requirements (`dkms`, `kmod`, PolicyKit/util-linux/desktop helpers) and must not depend on `fuse3` or `libfuse3-3`.

Installation fails if matching running-kernel headers are unavailable or if the native module cannot build/load/register. There is no userspace fallback.

`infilfs-inspect --udev` must identify a valid Format 0.15 volume with filesystem type, UUID, label/version and block-size properties and must remain silent for non-InfiltratorFS input.

## Automated gates

The main Build and conformance workflow runs the portable/core suite under GCC, Clang, ASan/UBSan and GCC `-fanalyzer`, including deterministic and randomized bitmap-oracle qualification of the rebuildable free-extent index. It validates desktop/Manager behavior, checks cross-platform Linux-created media on Windows, builds release packages and rejects any FUSE build artifact or package dependency.

Portable smoke qualification requires a 510-byte UTF-8 component to succeed and a 511-byte component to be rejected. Native mounted qualification additionally verifies that `statfs` advertises the 510-byte boundary and repeats create/read/enumerate/reject through the Linux VFS.

The Native Linux kernel module workflow builds the out-of-tree driver, reproduces the DKMS source-root invocation, validates module metadata and performs mounted snapshot, all-namespace xattr, special-node, mmap, namespace, allocation-reporting, 4,000-write random I/O, repeated-fsync, bounded near-full fragmentation/refill allocation, crash-orphan recovery, checkpoint fallback/healing, remount and scrub qualification when the hosted runner exposes matching running-kernel headers.

The release publisher runs only after a successful `Release <version>` commit on current `main`. It rebuilds assets from that exact commit, installs the generated `.deb`, verifies the native filesystem registration, mounts a real Format 0.15 loop image with `FSTYPE=infiltratorfs`, writes and byte-compares non-zero data, syncs, unmounts, requires a CLEAN scrub and rejects any legacy FUSE executable/process before creating the tag and release.
