<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS

InfiltratorFS is a clean-sheet, platform-neutral general-purpose filesystem started in 2026. The persistent format and core engine are written in portable C; Linux and Windows are adapters over the same on-disk structures rather than separate filesystem implementations.

## Current status — implementation 0.9.11 / disk format 0.8

Format 0.8 is unchanged by the 0.9.x implementation releases. Media created by earlier Format 0.8 builds does **not** need to be reformatted to use 0.9.11.

Implementation 0.9.11 focuses on portability reuse, recovery safety and real-device I/O behaviour:

- consumes **Infiltratr Common 1.11.0** for fixed-width endian conversion, strict UTF-8 validation, exact EINTR-safe positioned POSIX I/O and checked/saturating arithmetic;
- keeps healthy real-device opens lightweight instead of performing a whole-filesystem scrub at mount time;
- requires complete graph validation before a writable opener heals mixed/degraded checkpoint replicas, so fast mounting cannot overwrite the last known-good recovery generation;
- rejects transaction-generation wrap instead of allowing `UINT64_MAX + 1` to fold to generation zero;
- removes redundant Windows `FILE_FLAG_WRITE_THROUGH`; durability remains controlled by the filesystem's explicit `FlushFileBuffers` transaction barriers;
- adds single-writer exclusion for Windows raw partition-region access while retaining bounded partition I/O;
- keeps the Windows bulk-copy adapter on a bounded deferred-publication window to avoid publishing the full allocation bitmap for every small write chunk; and
- preserves the 0.9.10 versioned Windows executable naming and embedded Windows File/Product version metadata.

Format 0.8 currently provides:

- 4096-byte little-endian blocks;
- nonzero 128-bit persistent filesystem and object identities;
- three physically separated checksummed checkpoints with generation-based recovery;
- authoritative bitmap allocation plus next-fit runtime allocation hints;
- transactional copy-on-write metadata/data publication;
- paged directory and object-index metadata for scalable entry capacity;
- inline small files and automatic inline/extent transitions;
- normal and hole extents, sparse growth and hole punching;
- shared extents/reflinks with CoW on later writes;
- CRC64-ECMA metadata integrity and SHA-256 file-data integrity;
- independently stored checksums for extent-backed data;
- portable timestamps/attributes and optional POSIX compatibility metadata;
- mandatory well-formed UTF-8 namespace components;
- namespace, ownership, checksum-chain and metadata graph validation;
- explicit read-only scrub/verify with file, data-block, checksum-error and metadata-error reporting;
- callback-based storage, durability, randomness and clock services; and
- operating-system-neutral `infs_status` values translated only at adapter boundaries.

## Platform model

| Layer | Status |
| --- | --- |
| On-disk format | Platform-neutral Format 0.8 |
| Core filesystem engine | Portable C17; no Linux fd/VFS or Win32 handle types |
| Shared foundations | Infiltratr Common 1.11.0 |
| Storage interface | Callback-based; POSIX, Win32 and in-memory test backends |
| Linux userspace adapter | Implemented through POSIX I/O and FUSE3 |
| Windows transfer/raw-volume adapter | Implemented; opens Format 0.8 partitions without a drive letter or Windows filesystem driver |
| Native Linux kernel driver | Future work |
| Native Windows filesystem driver / Explorer mount | Future work |

The Windows transfer application is **not** a Windows kernel filesystem driver. It discovers raw physical partitions, opens InfiltratorFS directly, lists the current root directory, copies files/folders and runs a full scrub. Explorer drive-letter mounting requires a future Windows filesystem driver, but no Format 0.8 conversion or reformat is intended to be necessary.

## Infiltratr Common dependency

InfiltratorFS pins Infiltratr Common 1.11.0. A parent/installed `InfiltratrCommon` package may provide the targets; otherwise the repository submodule is used. CMake also has an exact-commit FetchContent fallback for source archives where Git submodules are unavailable.

The filesystem-specific transaction, allocation, namespace, checksum and on-disk-format rules remain inside InfiltratorFS. Common owns only generally reusable primitives.

## Build on Linux Mint

```bash
sudo apt install build-essential cmake pkg-config libfuse3-dev fuse3
git submodule update --init --recursive
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

Mount read/write with FUSE:

```bash
mkdir -p /tmp/infilfs-mnt
./build/infilfs-fuse infilfs.img /tmp/infilfs-mnt -f
```

Mount read-only:

```bash
./build/infilfs-fuse infilfs.img /tmp/infilfs-mnt -f -o ro
```

The FUSE adapter remains deliberately single-threaded while the current core is single-writer. POSIX storage uses shared locks for read-only openers and an exclusive lock for writers/formatters. Explicit `fsync` and final unmount publish any deferred transaction.

## Linux Mint desktop manager

The Debian package installs **InfiltratorFS Manager**. It can create/format images, select removable partitions, format with destructive-action confirmation, inspect/scrub, mount through FUSE, open in Nemo and unmount safely. Mint Disks may display an InfiltratorFS partition as unknown because InfiltratorFS is not yet registered as a native UDisks filesystem type.

## Windows transfer application

The Windows release contains a versioned executable such as:

```text
InfiltratorFS-Windows-0.9.11.exe
```

Run it elevated when accessing raw media. It can discover normal Windows volumes and SD/MMC/USB partitions directly from their physical partition tables, including a Format 0.8 partition with no drive letter. Writable raw-partition opens are bounded to the selected partition and use cooperative single-writer exclusion. Transaction durability is provided by explicit filesystem flush barriers rather than write-through on every individual block write.

## Recovery and scrub

A normal healthy block-device open validates the checkpoints, bitmap and essential root/index state needed for operation without walking every file. Full namespace/ownership/checksum graph verification belongs to **Scrub / Verify**.

Recovery is stricter. If a writable block-device open sees checkpoint replicas that are missing or disagree, each candidate is fully graph-validated before any checkpoint copy is healed. A corrupt newer graph may fall back to an older valid committed generation; an unreadable checkpoint location remains a hard writable-open error because it might contain the only durable newer generation.

## Repository layout

```text
include/infilfs/          public format/storage/filesystem interfaces
src/                      portable filesystem core
src/infiltratr-common/    pinned Infiltratr Common submodule
src/platform/             POSIX and Win32 storage adapters
tools/                    formatter, inspector, scrubber and transfer tools
fuse/                     Linux FUSE3 adapter
tests/                    conformance, recovery, crash and platform tests
docs/                     architecture and on-disk-format documentation
```

## Safety

InfiltratorFS remains experimental. Keep backups and use disposable/test media while developing it.

The FUSE adapter currently resolves operations by pathname rather than retaining complete persistent open-inode state. POSIX open-handle behaviour across unlink/rename is therefore not yet the final kernel-filesystem model.

## License

GPL-3.0-or-later.
