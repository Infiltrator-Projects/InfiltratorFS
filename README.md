<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# InfiltratorFS

[![Build and conformance](https://github.com/Infiltrator-Projects/InfiltratorFS/actions/workflows/ci.yml/badge.svg)](https://github.com/Infiltrator-Projects/InfiltratorFS/actions/workflows/ci.yml)

InfiltratorFS is a clean-sheet, platform-neutral general-purpose filesystem. The persistent format and portable core define the filesystem; Linux, Windows and future operating-system adapters map their native APIs onto the same objects, transactions, extents, snapshots and integrity model.

<!--
Release-policy compatibility anchor. This line is deliberately hidden from the
user-facing README so visible status wording can change without breaking CI.
**Current source version:** 0.18.40 (Format 0.17)<br>
-->
**Current source:** 0.18.40  
**On-disk format:** 0.17  
**Latest published release:** v0.18.39  
**Shared foundation:** Infiltratr Common 1.11.0  
**Licence:** GPL-3.0-or-later

Pre-1.0 development is current-format-only. Development-format compatibility is not promised, so test media may need reformatting after an on-disk-format revision.

The badge above is the broad build/portable conformance gate. Mounted native-Linux qualification is a separate evidence boundary; see `docs/QUALIFICATION.md`.

## What exists today

Format 0.17 provides:

- 4096-byte little-endian blocks and 128-bit filesystem/object identities;
- three physically separated checksummed checkpoints with generation-based recovery;
- copy-on-write transactions and retained historical generations;
- a sharded persistent allocation tree with rebuildable runtime free-extent indexes;
- generation-aware object and directory trees plus paged extent metadata;
- inline, sparse and shared/reflinked file data;
- symbolic links, hard links and named read-only snapshots;
- adaptive bounded per-extent compression using native IAC1 v1, with LZ4 retained as a non-default reference representation;
- CRC64-ECMA metadata integrity and SHA-256 logical file-data integrity;
- 1023-byte UTF-8 namespace components;
- portable attributes with operating-system-specific metadata isolated at adapter boundaries; and
- scrub, inspection and forensic tooling.

Linux is the most complete mounted adapter. The normal Linux path is the native out-of-tree `infiltratorfs.ko` VFS driver installed through DKMS; there is no current FUSE filesystem implementation or FUSE runtime fallback. The native driver includes the established read/write namespace surface, random and sparse I/O, truncate, `fallocate`, hole punching, FIEMAP/SEEK_DATA/SEEK_HOLE, reflinks, xattrs, special nodes, page cache/readahead, writable `mmap`, crash-safe open-unlink handling, checkpoint fallback/healing, online defragmentation, workload/media-aware allocation policy and online grow/bounded shrink.

Current development source also contains native user/group/project quota machinery. Quotas remain **unfinished as a roadmap capability until mounted qualification passes**. Resize is implemented and independently mounted-qualified. The authoritative feature status is `docs/ROADMAP.md`; exact evidence is `docs/QUALIFICATION.md`.

Windows currently provides native image/raw-partition access and a driverless Explorer bridge using Microsoft's inbox Projected File System (ProjFS). It is useful interoperability, but it is not a native InfiltratorFS Windows kernel driver. The native Windows filesystem driver remains future work.

## Linux quick start

Build on a Debian-family system with matching running-kernel headers:

```bash
sudo apt install build-essential cmake dkms kmod policykit-1 util-linux \
  xdg-utils python3 python3-gi gir1.2-gtk-3.0 udev udisks2 \
  linux-headers-$(uname -r)

git clone --recurse-submodules https://github.com/Infiltrator-Projects/InfiltratorFS.git
cd InfiltratorFS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
make -C kernel KDIR=/lib/modules/$(uname -r)/build
```

A normal native mount is:

```bash
sudo modprobe infiltratorfs
sudo mount -t infiltratorfs -o rw /dev/<partition> /mnt/infiltratorfs
findmnt -T /mnt/infiltratorfs -o SOURCE,FSTYPE,OPTIONS
```

`FSTYPE` must report `infiltratorfs`.

Create and inspect an image without mounting:

```bash
truncate -s 128M infilfs.img
./build/mkfs.infilfs -L test-volume infilfs.img
./build/infilfs-inspect infilfs.img
./build/infilfs-scrub infilfs.img
./build/infilfs-forensic --jsonl infilfs.img
```

For mounted fragmentation metrics and bounded online defragmentation:

```bash
./build/infilfs-optimize --metrics /mnt/infiltratorfs/file.bin
./build/infilfs-optimize --defrag /mnt/infiltratorfs/file.bin
./build/infilfs-optimize --defrag --recursive /mnt/infiltratorfs/tree
```

The destructive physical qualification harness remains available as `tests/native-complete-qualification.sh`; it is an explicit operator test, not ordinary CI.

## Desktop and packaging

Linux packages include the native module/DKMS integration, `mkfs.infiltratorfs`, inspection/scrub/forensic tools, `mount.infiltratorfs`, `fsck.infiltratorfs`, InfiltratorFS Manager, udev/UDisks identification and the repository's formatter-integration work for libblockdev/UDisks/GNOME Disks.

InfiltratorFS Manager formats and mounts selected non-system partitions through constrained privileged helpers. On Linux Mint, the project uses its own **Format partition as InfiltratorFS…** Nemo action rather than modifying Mintstick's whole-device formatting behaviour.

Published release assets are available from the repository's GitHub Releases page. Release publication performs its own installed-package native mount/scrub gate; milestone-scale million-file/1 TiB and endurance suites remain separate weekly/manual qualification rather than an automatic requirement for every release.

## Documentation ownership

To prevent documentation drift, each kind of fact has one authoritative home:

- `docs/ON_DISK_FORMAT.md` — persistent Format 0.17 layout and encoding contract.
- `docs/ARCHITECTURE.md` — design model and architectural invariants.
- `docs/ROADMAP.md` — **the only authoritative feature-completion list**.
- `docs/QUALIFICATION.md` — **the only authoritative exact-source qualification/evidence ledger**.
- `docs/PLATFORM_ADAPTERS.md` — operating-system adapter boundaries.
- `docs/SECURITY.md` — portable security/ACL design direction.
- `docs/COMPRESSION.md` — IAC1/compressed-extent design.
- `docs/FORENSICS.md` — forensic scanner model and use.
- `docs/INSPIRATIONS.md` — historical design influences, not project status.

Implementation comments and workflow comments should explain local behaviour only; they are not alternate project-status documents.

## Development rule

Before 1.0, prefer the cleanest long-term filesystem design over preserving obsolete development-format assumptions. A feature is marked complete only when its implementation and required qualification are both complete.