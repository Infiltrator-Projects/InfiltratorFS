<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS

InfiltratorFS is a clean-sheet, platform-neutral general-purpose filesystem started in 2026. It is written in C and has no requirement to preserve on-disk compatibility with FAT, NTFS, ext, Amiga FFS or any other legacy filesystem.

Linux is the first implementation and test platform. It is not part of the filesystem's identity: the on-disk format and core engine are designed so a future Windows implementation can use the same volume without conversion or reformatting.

## Current status — implementation 0.5.1 / format 0.5

Implementation 0.5.1 preserves the proven Format 0.5 transaction and integrity model, freezes its byte-level portability contract and removes operating-system error codes from the core boundary:

- 4096-byte little-endian on-disk format;
- 128-bit persistent filesystem and object identities;
- extent-based file allocation;
- authoritative free-space bitmap;
- three physically separated, checksummed checkpoints;
- transactional copy-on-write metadata, data and allocation state;
- atomic old-or-new generation publication without journal replay;
- CRC64-ECMA metadata checksums and SHA-256 file-data checksums;
- independently stored per-block data checksums;
- platform-neutral file attributes with birth, access, modification and metadata-change times;
- portable file flags plus references for future security and extended-attribute objects;
- POSIX permissions isolated as optional Linux adapter metadata;
- mandatory well-formed UTF-8 namespace components;
- callback-based storage, durable-flush, randomness and clock services;
- operating-system-neutral `infs_status` results with adapter mappings;
- byte-exact layout, malformed-image and deterministic in-memory volume conformance tests;
- automated Linux and Windows/MSVC core builds;
- Linux/POSIX I/O and FUSE kept outside the core engine.

The current prototype supports create, mkdir, lookup, enumeration, read, write, truncate, unlink, empty-directory removal, cross-directory rename, attribute updates, direct-image tools and full-volume read-only scrub.

## Platform model

| Layer | Status |
| --- | --- |
| On-disk format | Platform-neutral format 0.5 |
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
```

Mount with the current Linux adapter:

```bash
mkdir -p /tmp/infilfs-mnt
./build/infilfs-fuse infilfs.img /tmp/infilfs-mnt -f
```

The FUSE adapter remains single-threaded while the core is a single-writer prototype.

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
- `docs/ON_DISK_FORMAT.md` — complete format-0.5 specification.
- `docs/CONFORMANCE.md` — byte-level contract and cross-platform test requirements.
- `docs/ROADMAP.md` — completed work and future Linux/Windows integration.
- `docs/INSPIRATIONS.md` — ideas borrowed, rejected or reinterpreted.

## Safety

InfiltratorFS remains experimental. Use image files or disposable media only. Do not store irreplaceable data on format 0.5.

## License

GPL-3.0-or-later.
