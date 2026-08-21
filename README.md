<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS

InfiltratorFS is a clean-sheet, platform-neutral general-purpose filesystem started in 2026. It is written in C and has no requirement to preserve on-disk compatibility with FAT, NTFS, ext, Amiga FFS or any other legacy filesystem.

Linux is the first implementation and test platform. It is not part of the filesystem's identity: the on-disk format and core engine are designed so a future Windows implementation can use the same volume without conversion or reformatting.

## Current status — implementation 0.6.5 / format 0.6

Implementation 0.6.5 adds a Linux Mint desktop manager for image creation, formatting, inspection, scrubbing, mounting and opening volumes in the normal file manager. It preserves the recovery and concurrency hardening from 0.6.4 and does not change any packed field, feature bit or on-disk layout.

Current properties include:

- 4096-byte little-endian on-disk format;
- nonzero 128-bit persistent filesystem and object identities;
- normal and hole extents with zero-storage logical ranges;
- authoritative free-space bitmap with exact live-block ownership validation and minimum-coverage geometry checks before bitmap traversal;
- three physically separated, checksummed checkpoints;
- descending-generation checkpoint selection with complete referenced-graph validation and corruption-only fallback to an older committed generation;
- writable recovery that heals all readable checkpoint replicas to the selected committed generation and refuses to overwrite an unreadable replica;
- transactional copy-on-write metadata, data and allocation state;
- atomic old-or-new generation publication without journal replay;
- CRC64-ECMA metadata checksums and SHA-256 file-data checksums;
- independently stored per-block data checksums;
- sparse checksum chains that allocate metadata only for stored data;
- checksum-chain reachability validation so hidden checksum metadata cannot be orphaned or shared accidentally;
- platform-neutral file attributes with birth, access, modification and metadata-change times;
- portable file flags plus reserved references for future security and extended-attribute objects;
- POSIX permissions isolated as optional Linux adapter metadata;
- mandatory well-formed UTF-8 namespace components;
- root-to-leaf namespace validation for unique names, target identity/type, parent links, link counts and reachability;
- canonical zero/reserved-field validation for the current Format 0.6 contract;
- callback-based storage, durable-flush, randomness and clock services, with writable opens requiring every mutation-critical service;
- fail-closed close-and-reopen recovery after a commit-checkpoint write or flush whose outcome is indeterminate;
- operating-system-neutral `infs_status` results preserved through public mutation/read helpers and translated only at adapter boundaries;
- ordinary POSIX rename replacement semantics, including trailing-slash directory requirements;
- genuinely read-only backing storage for FUSE `-r` / `-o ro` mounts;
- normalized pre-epoch FUSE timestamps and overflow-checked `utimens` conversion;
- block-device formatting that refuses mounted/held targets, acquires an exclusive Linux block-device open before destructive writes, and fails if the initial realtime clock query fails;
- shared POSIX storage locks for concurrent read-only tools and an exclusive lock for writers and the formatter;
- formatter publication ordering that leaves an interrupted format unmountable rather than publishing checkpoints before referenced metadata is durable;
- byte-exact layout, malformed-image and deterministic in-memory volume conformance tests;
- explicit regressions for undersized allocation bitmaps, incomplete writable backends, checkpoint-graph fallback, non-masked recovery I/O failures and rename trailing slashes;
- high-offset sparse write, hole-punch, crash-atomicity and reclamation tests;
- automated Linux, Windows/MSVC, Clang, ASan/UBSan and GCC `-fanalyzer` gates;
- Linux/POSIX I/O and FUSE kept outside the portable core engine.

The current prototype supports create, mkdir, lookup, enumeration, read, write, sparse grow, truncate, hole punch, unlink, empty-directory removal, atomic rename/replacement, attribute updates, direct-image tools and full-volume read-only scrub.

## Platform model

| Layer | Status |
| --- | --- |
| On-disk format | Platform-neutral format 0.6 |
| Core filesystem engine | Portable C17 with no Linux file descriptor or VFS types |
| Storage interface | Callback-based and tested with POSIX files/devices and an in-memory backend |
| Result interface | Stable `infs_status` values; native errors are adapter translations |
| Linux userspace adapter | Implemented through POSIX I/O and FUSE3 |
| Native Linux kernel driver | Planned |
| Windows storage adapter and native driver | Planned; no format redesign should be required |

## Build on Linux Mint

```bash
sudo apt install build-essential cmake pkg-config libfuse3-dev fuse3
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Create and inspect an image:

```bash
truncate -s 128M infilfs.img
./build/mkfs.infilfs -L test-volume infilfs.img
./build/infilfs-inspect infilfs.img
./build/infilfs-scrub infilfs.img
```

Exercise the filesystem without mounting:

```bash
./build/infilfs-tool infilfs.img mkdir /docs
./build/infilfs-tool infilfs.img put README.md /docs/README.md
./build/infilfs-tool infilfs.img cat /docs/README.md
./build/infilfs-tool infilfs.img punch /docs/README.md 4096 4096
```

Mount read/write with the current Linux adapter:

```bash
mkdir -p /tmp/infilfs-mnt
./build/infilfs-fuse infilfs.img /tmp/infilfs-mnt -f
```

Mount read-only without opening the backing image/device writable:

```bash
./build/infilfs-fuse infilfs.img /tmp/infilfs-mnt -f -o ro
```

## Linux Mint desktop manager

The Debian package installs **InfiltratorFS Manager** in Mint's application menu. It can:

- create and format an image file;
- select a removable partition created in Mint Disks;
- format a selected target with an explicit destructive-action confirmation;
- inspect or scrub a volume;
- mount it and open it in Nemo; and
- unmount it safely.

Mint Disks remains responsible for creating the disk partition. The manager deliberately lists removable partitions only and uses PolicyKit authorization for operations that need raw-device access. InfiltratorFS is not yet registered as a filesystem type with UDisks, so Mint Disks itself will continue to display a formatted partition as unknown.

The FUSE adapter remains single-threaded while the core is a single-writer prototype. The POSIX backend now enforces that model: multiple read-only openers may coexist, while a writable opener or formatter holds the storage target exclusively. On Linux Mint, `bash tests/mint-sparse-fuse.sh build` runs the real mounted 1 TiB sparse-file/write/punch/remount harness after the normal CTest suite.

## Repository layout

```text
include/infilfs/     portable format, storage and filesystem interfaces
src/                 portable checksum, storage and filesystem core
src/platform/        current platform adapters
tools/               formatter, inspector, scrubber and direct-image utility
fuse/                Linux FUSE3 adapter
tests/               integrity, crash, namespace and portable-backend tests
docs/                architecture, format, roadmap and design inspirations
```

## Documentation

- `docs/ARCHITECTURE.md` — platform model, transaction design and long-term rules.
- `docs/ON_DISK_FORMAT.md` — complete format-0.6 specification.
- `docs/CONFORMANCE.md` — byte-level contract and cross-platform test requirements.
- `docs/ROADMAP.md` — completed work and future Linux/Windows integration.
- `docs/INSPIRATIONS.md` — ideas borrowed, rejected or reinterpreted.

## Safety

InfiltratorFS remains experimental. Use image files or disposable media only. Do not store irreplaceable data on Format 0.6.

The current high-level FUSE adapter resolves operations by pathname and does not yet retain persistent inode/open-file state. POSIX open-handle semantics across unlink and rename are therefore incomplete; applications that keep descriptors open while names move or disappear can observe incorrect behaviour. Treat that workload as unsupported until the adapter has explicit handle-lifetime tests and implementation.

## License

GPL-3.0-or-later.

