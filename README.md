<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS

InfiltratorFS is a clean-sheet experimental filesystem for Linux, started in 2026. It is not constrained by compatibility with FAT, NTFS, ext, Amiga FFS, or any other on-disk format.

The project goal is to combine the strongest ideas from historical and modern filesystems into a small, understandable, recoverable design whose on-disk structures are explicitly versioned and self-describing.

## Design principles

- Transactional metadata: committed filesystem state is never half-written.
- Copy-on-write metadata from the beginning.
- 128-bit persistent object identifiers rather than reusable inode numbers.
- Extent-based file allocation; no FAT-style cluster chains.
- Hybrid free-space tracking: allocation bitmap plus extent-oriented free-space indexing.
- Checksummed metadata and, as the format matures, checksummed file data.
- Multiple distributed filesystem checkpoints.
- Self-identifying metadata structures for forensic reconstruction.
- Native generations, snapshots, reflinks, sparse extents, inline small files and historical recovery as first-class format concepts.
- Adaptive allocation policies for HDD, SSD, NVMe and removable flash.
- No requirement that every workload use copy-on-write data semantics; metadata remains transactional while file-data policy can be workload-aware.
- Linux-first implementation in C.

## Current status — format 0.1 prototype

The repository already contains the first real on-disk prototype:

- 4096-byte filesystem blocks.
- three superblock/checkpoint copies at the start, midpoint and end of the volume;
- CRC64-ECMA metadata integrity checking;
- a free-space allocation bitmap;
- a 128-bit filesystem UUID;
- a 128-bit root object ID;
- a checksummed root-directory object;
- `mkfs.infilfs` to format a file-backed image or, with an explicit safety flag, a block device;
- `infilfs-inspect` to validate and inspect the filesystem;
- an optional FUSE3 mount target that mounts the valid prototype read-only and exposes the empty root directory.

This is deliberately the smallest useful vertical slice. File creation, directory entries and extents are the next implementation layer.

## Build on Linux Mint

```bash
sudo apt install build-essential cmake pkg-config libfuse3-dev
cmake -S . -B build
cmake --build build
```

Create and inspect a test filesystem image:

```bash
truncate -s 128M infilfs.img
./build/mkfs.infilfs -L test-volume infilfs.img
./build/infilfs-inspect infilfs.img
```

Mount the current read-only prototype:

```bash
mkdir -p /tmp/infilfs-mnt
./build/infilfs-fuse infilfs.img /tmp/infilfs-mnt -f
```

The FUSE target is intentionally thin. The filesystem engine and on-disk format do not depend on FUSE, allowing a future native Linux filesystem driver to use the same format and core concepts.

## Documentation

- `docs/ARCHITECTURE.md` — overall architecture and design rules.
- `docs/ON_DISK_FORMAT.md` — current format 0.1 structures and invariants.
- `docs/ROADMAP.md` — implementation phases.
- `docs/INSPIRATIONS.md` — ideas borrowed, rejected or reinterpreted from other filesystems.

## Safety

InfiltratorFS is experimental. Use image files or disposable media only. Do not store irreplaceable data on the prototype.

## License

GPL-3.0-or-later.
