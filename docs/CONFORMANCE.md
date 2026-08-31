<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Format 0.17 Conformance

Release 0.18.29 accepts exactly current on-disk Format 0.17. Pre-1.0 builds do not promise compatibility with earlier development formats.

## Persistent representation

A conforming implementation preserves:

- 4096-byte little-endian filesystem blocks;
- exact Format 0.17 major/minor identity;
- the packed structure sizes and offsets declared by the public format headers;
- three physical checkpoint locations;
- CRC64-ECMA protection for checkpoints and metadata;
- SHA-256 protection for logical file data;
- nonzero 128-bit filesystem and object identities;
- nonzero committed generations;
- valid UTF-8 labels and namespace components with canonical reserved/padding bytes;
- case-sensitive byte-exact namespace comparison;
- authoritative one-bit-per-block allocation ownership persisted through the Format 0.17 allocation tree;
- independently CRC64-protected allocation branch/leaf pages with exact logical index, level and entry-count validation;
- transaction allocation/deferred-range journals rather than whole-volume bitmap rollback clones;
- committed allocation-layout caching in both portable and native implementations, with replacement only after the primary checkpoint durability boundary;
- native statfs accounting from the active deferred transaction, with internal metadata reserve excluded from f_bavail;
- adaptive native write-chunk subdivision so fragmented free space is consumed down to one-block runs before ENOSPC is reported;
- exact root/object-index/directory/checksum graph reachability;
- exact link counts and parent/reference rules;
- ordinary extents, sparse hole extents and shared normal extents;
- inline small-file representation;
- generation-aware object-index and directory trees plus paged extent metadata;
- symbolic-link objects;
- regular-file hard links;
- snapshot-catalog objects and retained historical generations; and
- common portable attributes plus isolated POSIX compatibility metadata.

Unknown incompatible feature bits are rejected. Unknown read-only-compatible bits may be accepted only read-only. Newly created Format 0.17 volumes enable the currently required known feature set.

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

Critical committed metadata is copy-on-write. Allocation bits remain authoritative, but the live persistent representation is a sharded allocation tree. A mutation publishes a new generation only after changed allocation leaves, the replacement internal allocation-tree path, the replacement object/namespace graph and required durability operations are complete.

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
- correct lookup/enumeration of current Format 0.17 directories and object indexes;
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
- bounded multi-process contention across same-directory create/rename/link/unlink, shared-inode disjoint writes and fsync, same-inode xattr readers/writers, stable concurrent reads and open-unlink writers without transient structural errors;
- mounted scale stress with at least 1,000,000 distinct regular files across a bounded directory fan-out, 100,000 unlink/recreate operations, durable read-only remount verification and a CLEAN offline scrub;
- mounted large-volume stress on a 1 TiB sparse loop-backed Format 0.17 volume, including non-zero extent I/O, thousands of namespace objects, a 900 GiB sparse high-offset file, read-only remount verification and a CLEAN offline scrub;
- bounded near-full endurance on a 4 GiB mounted Format 0.17 image with multiple fragmentation/refill size classes, a second free-run fragmentation pass, an explicit hole-punch/refill cycle, and a five-minute concurrent mixed workload combining random 4 KiB overwrites/readback, appends, rename churn, xattrs, sparse truncate/high-offset writes, hard/symbolic links and repeated fsync; durable content hashes and namespace metadata must survive an offline CLEAN scrub, read-only remount verification and a second CLEAN scrub;
- native per-file fragmentation metrics reporting logical size, allocated blocks, data/hole extent counts, largest physical run, generation and a normalized fragmentation score; and bounded copy-on-write online defragmentation that relocates only logically adjacent normal extents, preserves file identity/content/xattrs/links/timestamps, uses snapshot/reflink-aware old-block release, publishes through the native transaction path and survives CLEAN scrub/remount qualification;
- rejection of cyclic or multiply aliased checksummed metadata trees without mount hang or pathological repeated traversal;
- no allocation-map/tree helper may require a multi-kilobyte automatic kernel-stack scratch array;
- package configuration must complete without loading the module when an explicit administrator `modprobe install ... /bin/false` safety override is active;
- clean unmount followed by userspace scrub; and
- refusal to silently substitute a non-native filesystem path.

Release 0.18.29 qualifies this native migrated surface, including concurrent
64-shard data-run reservation before the serialized metadata transaction.
Additional development qualification is expected to expand workload-aware
placement and long-running mixed-workload stress rather than reintroduce a FUSE
runtime path.

`mount.infiltratorfs` and InfiltratorFS Manager must produce `FSTYPE=infiltratorfs`. The Manager privileged helper rejects a mounted result with any other filesystem type.

## 2026-08-29 checked-roadmap qualification

The 56 roadmap entries marked complete at source commit
`075aed9c737fb38cc408d752736a97773dc2a035` were subjected to a full
portable/CI/physical-media audit on 2026-08-29. The maintained physical native
VFS harness passed 69/69 checks with zero warnings, four additional mounted
concurrency rounds passed, and the final post-concurrency offline scrub at
generation 3695 checked 1,446 files and 151,580 data blocks with zero checksum
or metadata errors. No corruption/lockup-class kernel diagnostic was present
in the corrected qualification interval.

The full evidence record, performance telemetry and audit-wrapper corrections
are retained in [QUALIFICATION.md](QUALIFICATION.md).

## Adapter conformance

Operating-system adapters may expose different native APIs, but they must not redefine the persistent meaning of shared filesystem concepts. Equivalent native concepts map to the same portable object/extent/transaction semantics. Adapter-only metadata must remain isolated and must not be silently discarded merely because another adapter cannot expose it.

POSIX UID/GID/mode compatibility metadata is not the final cross-platform security authority. The future security-object model is defined architecturally in `SECURITY.md`; until that format exists, adapters must not claim portable ACL/SID equivalence that is not actually stored.

## Desktop/package conformance

The Linux package must contain the formatter, inspector, direct-image tool, scrubber, forensic scanner, native fragmentation/optimizer tool, Manager, constrained helper, standard mount/fsck helpers, udev rule and self-contained DKMS source.

The package must depend on the native build/runtime requirements (`dkms`, `kmod`, PolicyKit/util-linux/desktop helpers) and must not depend on `fuse3` or `libfuse3-3`.

Installation fails if matching running-kernel headers are unavailable or if the native module cannot build/load/register. There is no userspace fallback.

`infilfs-inspect --udev` must identify a valid Format 0.17 volume with filesystem type, UUID, label/version and block-size properties and must remain silent for non-InfiltratorFS input.

## Automated gates

The main Build and conformance workflow runs the portable/core suite under GCC, Clang, ASan/UBSan and GCC `-fanalyzer`, including deterministic and randomized bitmap-oracle qualification of the rebuildable free-extent index and a policy guard that forbids restoration of monolithic persistent bitmap publication or whole-bitmap transaction clones. It validates desktop/Manager behavior, checks cross-platform Linux-created media on Windows, and—when Microsoft's ProjFS component is available on the Windows runner—starts the driverless Explorer provider and requires an external Windows client to read Linux-created data and persist create/write/rename/hard-link/delete operations back through the portable core. It also builds release packages and rejects any FUSE build artifact or package dependency.

Portable smoke qualification requires a 1023-byte UTF-8 component to succeed and a 1024-byte component to be rejected. Native mounted qualification additionally verifies that `statfs` advertises the 1023-byte boundary and repeats create/read/enumerate/reject through the Linux VFS.

The Native Linux kernel module workflow builds the out-of-tree driver, reproduces the DKMS source-root invocation, validates module metadata and performs mounted snapshot, all-namespace xattr, special-node, mmap, namespace, multi-process locking/concurrency, allocation-reporting, 4,000-write random I/O, repeated-fsync, bounded near-full fragmentation/refill allocation, crash-orphan recovery, checkpoint fallback/healing, malicious metadata-alias rejection with an explicit mount timeout, remount and scrub qualification when the hosted runner exposes matching running-kernel headers.

A dedicated scale-stress job runs only after the ordinary native kernel qualification succeeds. It creates one million distinct files, churns 100,000 of them through unlink/recreate, verifies the durable population after a read-only remount, requires a CLEAN scrub, and separately mounts and qualifies a 1 TiB sparse loop-backed volume. The workload emits `[SCALE-PERF]` telemetry so scale regressions are observable rather than reduced to a binary pass/fail.

A dedicated endurance job independently drives a 4 GiB native volume below 15 percent free space while keeping a safety reserve, manufactures multiple physical free-run size classes, runs a five-minute multi-process mixed metadata/data workload, adds an explicit hole-punch/refill cycle, and requires two CLEAN offline scrubs around a read-only durable-manifest verification. It emits `[ENDURANCE-PERF]` telemetry for fragmentation setup, operation rate, fsync latency, free-space floor, sync and scrub timing.

The native optimizer qualification deliberately fragments a mounted extent-backed file through durable 4 KiB CoW overwrites, records pre-optimization metrics, runs `infilfs-optimize --defrag`, requires the data-extent count to fall, verifies byte-identical content, hard-link inode identity, xattr retention and unchanged modification time, and then requires a CLEAN offline scrub plus read-only remount verification.

The release publisher runs only after a successful `Release <version>` commit on current `main`. It rebuilds assets from that exact commit, installs the generated `.deb`, verifies the native filesystem registration, mounts a real Format 0.17 loop image with `FSTYPE=infiltratorfs`, writes and byte-compares non-zero data, syncs, unmounts, requires a CLEAN scrub and rejects any legacy FUSE executable/process before creating the tag and release.
