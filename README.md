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

## Current status — format 0.3 / Phase 2 transaction core

Format 0.3 keeps the writable Phase 1 namespace and extent engine and adds the first crash-consistency model:

- all modified metadata objects are written to newly allocated blocks;
- object-index updates are themselves copy-on-write;
- Linux `st_ino` values are derived from persistent object IDs rather than physical metadata blocks, so CoW relocation does not change inode identity;
- the allocation bitmap is copy-on-write rather than overwritten in place;
- allocation reservations remain unreachable until checkpoint publication;
- blocks superseded by a transaction are not reusable until the new checkpoint is durable;
- one physically separated checkpoint is published as the atomic commit point;
- the remaining checkpoint copies are then replicated;
- after a crash between checkpoint copies, a writable open heals all copies to the newest fully validated generation before allocating again;
- simulated crashes before bitmap publication, after bitmap publication and immediately after the first checkpoint publication are covered by automated tests.

Format 0.3 still uses a one-block object index and one-block directories as deliberate prototype limits. Those become scalable trees later without changing 128-bit object identity.

### Crash-consistency boundary

Format 0.3 protects filesystem **metadata publication** and newly allocated blocks from half-committed namespace/allocation state. Existing file-data blocks may still be overwritten in place, so an application overwriting bytes inside an already allocated extent does not yet receive old-or-new data atomicity. End-to-end data checksums and stronger data-CoW policies are later work.

## Implemented filesystem operations

- create and mkdir;
- lookup and directory enumeration;
- read and write;
- grow/shrink truncate with zero-fill semantics;
- unlink and empty-directory removal;
- same-directory and cross-directory rename;
- mode, ownership and nanosecond timestamps;
- direct-image tools and a writable FUSE3 front end.

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
- `docs/ON_DISK_FORMAT.md` — current format 0.3 structures and transaction protocol.
- `docs/ROADMAP.md` — implementation phases and completion state.
- `docs/INSPIRATIONS.md` — ideas borrowed, rejected or reinterpreted from other filesystems.

## Safety

InfiltratorFS is experimental. Use image files or disposable media only. Do not store irreplaceable data on format 0.3.

## License

GPL-3.0-or-later.
