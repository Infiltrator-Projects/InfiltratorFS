<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS

InfiltratorFS is a clean-sheet experimental filesystem for Linux, started in 2026. It has no requirement to preserve on-disk compatibility with FAT, NTFS, ext, Amiga FFS, or another legacy filesystem.

The project is intended to combine strong ideas from historical and modern filesystems into an understandable, recoverable design with explicitly versioned on-disk structures. The implementation is Linux-first and written in C.

## Design direction

- 128-bit persistent object identifiers rather than reusable inode numbers.
- Extent-based file allocation; no FAT-style cluster chains.
- Authoritative free-space bitmap with a future rebuildable free-extent accelerator.
- Checksummed, self-identifying metadata.
- Multiple physically separated filesystem checkpoints.
- Transactional copy-on-write metadata as the Phase 2 crash-consistency model.
- End-to-end data integrity, snapshots, reflinks, sparse extents, inline small files and historical recovery in later phases.
- Allocation policies that can eventually adapt to HDD, SSD, NVMe and removable flash.
- No dependency on FUSE in the on-disk format or core engine.

## Current status — format 0.2 / Phase 1

Format 0.2 is the first writable prototype. It implements:

- 4096-byte filesystem blocks;
- three checksummed superblock/checkpoint copies at block 0, the midpoint and the final block;
- CRC64-ECMA metadata corruption detection;
- authoritative one-bit-per-block allocation bitmap;
- 128-bit filesystem UUIDs and persistent object IDs;
- a persistent object index mapping object ID to physical metadata block;
- variable-length directory entries mapping names to object IDs;
- regular files backed by one or more physical extents;
- file and directory mode bits, UID, GID, link count, size and timestamps;
- create, mkdir, lookup, read, write, truncate, unlink, rmdir and rename;
- cross-directory moves with persistent parent-object relationships;
- bounds checks and hard rejection of inconsistent/checksum-invalid metadata;
- `mkfs.infilfs`, `infilfs-inspect` and `infilfs-tool`;
- a writable FUSE3 front end using the same core engine;
- automated persistence, zero-fill, rename, deletion and corruption tests.

Phase 1 intentionally updates metadata in place. It is **not yet crash-consistent**. Transactional copy-on-write metadata and generation publication are Phase 2 work.

Current format-0.2 scalability limits are deliberate prototype limits: the object index occupies one metadata block and each directory occupies one metadata block. These structures are designed to become scalable trees in later phases without changing object identity.

## Build on Linux Mint

```bash
sudo apt install build-essential cmake pkg-config libfuse3-dev fuse3
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Create a test filesystem image:

```bash
truncate -s 128M infilfs.img
./build/mkfs.infilfs -L test-volume infilfs.img
./build/infilfs-inspect infilfs.img
```

Exercise the filesystem without mounting it:

```bash
./build/infilfs-tool infilfs.img mkdir /docs
./build/infilfs-tool infilfs.img put README.md /docs/README.md
./build/infilfs-tool infilfs.img ls /docs
./build/infilfs-tool infilfs.img cat /docs/README.md
```

Mount it through FUSE3:

```bash
mkdir -p /tmp/infilfs-mnt
./build/infilfs-fuse infilfs.img /tmp/infilfs-mnt -f
```

Then ordinary Linux programs can operate on the mounted namespace. The Phase 1 FUSE front end runs single-threaded deliberately while the core remains a single-writer prototype.

## Repository layout

```text
include/infilfs/     on-disk structures and core APIs
src/                 checksum, block I/O and filesystem engine
tools/               formatter, inspector and direct-image utility
fuse/                Linux FUSE3 front end
tests/               persistence/API/corruption tests
docs/                architecture, format and roadmap
```

## Documentation

- `docs/ARCHITECTURE.md` — design model and long-term rules.
- `docs/ON_DISK_FORMAT.md` — current format 0.2 structures and invariants.
- `docs/ROADMAP.md` — implementation phases and current completion state.
- `docs/INSPIRATIONS.md` — ideas borrowed, rejected or reinterpreted from other filesystems.

## Safety

InfiltratorFS is experimental. Use image files or disposable media only. Do not store irreplaceable data on format 0.2.

## License

GPL-3.0-or-later.
