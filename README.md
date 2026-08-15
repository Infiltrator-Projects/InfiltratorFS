<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS

InfiltratorFS is a clean-sheet experimental filesystem for Linux, started in 2026. It has no requirement to preserve on-disk compatibility with FAT, NTFS, ext, Amiga FFS, or another legacy filesystem. The implementation is Linux-first and written in C.

## Design direction

- 128-bit persistent object identifiers rather than reusable inode numbers.
- Extent-based file allocation; no FAT-style cluster chains.
- Authoritative free-space bitmap with a future rebuildable free-extent accelerator.
- Checksummed, self-identifying metadata.
- Multiple physically separated filesystem checkpoints.
- Transactional copy-on-write metadata with atomic generation publication.
- End-to-end data integrity, snapshots, reflinks, sparse extents, inline small files and historical recovery in later phases.
- Allocation policies that can eventually adapt to HDD, SSD, NVMe and removable flash.
- No dependency on FUSE in the on-disk format or core engine.

## Current status — format 0.4 / Phase 3 data integrity core

Format 0.4 extends the Phase 2 transaction model to ordinary file-data overwrites and adds end-to-end data verification:

- every allocated logical file block has an independently stored checksum;
- checksum records live in hidden 128-bit checksum objects addressed through the persistent object index;
- checksum entries reserve 32 bytes each so stronger 256-bit algorithms can be introduced later without redesigning the checksum-object layout;
- SHA-256 is the format-0.4 file-data checksum algorithm and occupies the full 32-byte checksum entry;
- normal reads verify every full 4096-byte data block before returning bytes to the caller;
- writes to committed data blocks allocate replacement blocks rather than overwriting generation N in place;
- file extent metadata and checksum objects are published atomically with the same generation checkpoint as the data;
- pre-commit crashes retain the old bytes, while post-checkpoint crashes expose the new bytes;
- read-only `infilfs-scrub` walks all regular-file blocks and reports checksum or checksum-metadata errors;
- deliberate single-byte data corruption is detected by both the normal read path and scrubber;
- repeated data-CoW overwrites reclaim superseded data/checksum metadata without leaking space.

Format 0.4 still uses a one-block object index and one-block directories as deliberate prototype limits. Metadata still uses CRC64-ECMA in format 0.4, while file data uses SHA-256. Repair from redundant data copies and forensic reconstruction remain later integrity work.

## Implemented filesystem operations

- create and mkdir;
- lookup and directory enumeration;
- read and write;
- grow/shrink truncate with zero-fill semantics;
- unlink and empty-directory removal;
- same-directory and cross-directory rename;
- mode, ownership and nanosecond timestamps;
- direct-image tools, `infilfs-scrub`, and a writable FUSE3 front end.

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

Exercise it without mounting:

```bash
./build/infilfs-tool infilfs.img mkdir /docs
./build/infilfs-tool infilfs.img put README.md /docs/README.md
./build/infilfs-tool infilfs.img ls /docs
./build/infilfs-tool infilfs.img cat /docs/README.md
```

Mount through FUSE3:

```bash
mkdir -p /tmp/infilfs-mnt
./build/infilfs-fuse infilfs.img /tmp/infilfs-mnt -f
```

The FUSE implementation remains deliberately single-threaded while the core is a single-writer prototype.

## Repository layout

```text
include/infilfs/     on-disk structures and core APIs
src/                 checksum, block I/O and filesystem engine
tools/               formatter, inspector and direct-image utility
fuse/                Linux FUSE3 front end
tests/               persistence, corruption and crash-injection tests
docs/                architecture, format and roadmap
```

## Documentation

- `docs/ARCHITECTURE.md` — design model and long-term rules.
- `docs/ON_DISK_FORMAT.md` — current format 0.4 structures, checksum objects and transaction protocol.
- `docs/ROADMAP.md` — implementation phases and completion state.
- `docs/INSPIRATIONS.md` — ideas borrowed, rejected or reinterpreted from other filesystems.

## Safety

InfiltratorFS is experimental. Use image files or disposable media only. Do not store irreplaceable data on format 0.4.

## License

GPL-3.0-or-later.
