<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# InfiltratorFS

[![Build and conformance](https://github.com/Infiltrator-Projects/InfiltratorFS/actions/workflows/ci.yml/badge.svg)](https://github.com/Infiltrator-Projects/InfiltratorFS/actions/workflows/ci.yml)

InfiltratorFS is a clean-sheet, platform-neutral general-purpose filesystem. The persistent format and portable core define the filesystem; Linux, Windows and future operating-system adapters map their native APIs onto the same objects, transactions, extents, snapshots and integrity model.

<!--
Release-policy compatibility anchor. This line is deliberately hidden from the
user-facing README so visible status wording can change without breaking CI.
**Current source version:** 0.18.64 (Format 0.18)<br>
-->
**Current source:** 0.18.64<br>
**On-disk format:** 0.18<br>
**Published releases:** [GitHub Releases](https://github.com/Infiltrator-Projects/InfiltratorFS/releases)<br>
**Shared foundation:** Infiltratr Common 1.19.8<br>

The Manager's appearance contract is shared with the other desktop applications: **Day** is the white Infiltrator palette, **Night** is the MB graphite/black palette with the canonical blue accent, and **System** follows the host light/dark preference by selecting exactly one of those two palettes.
**Licence:** GPL-3.0-or-later

Pre-1.0 development is current-format-only. Development-format compatibility is not promised, so test media may need reformatting after an on-disk-format revision.

The badge above is the broad build/portable conformance gate. Mounted native-Linux qualification is a separate evidence boundary; see `docs/QUALIFICATION.md`.

## Engineering ethos

What happens when you build a filesystem from first principles?

InfiltratorFS is where questions about storage become code: what survives a crash, how old versions are retained, and what it takes to boot and run a real Linux installation.

The persistent format, transaction model, recovery rules, namespace, allocation policy, integrity model and history semantics are owned by this project rather than delegated to another filesystem implementation. Operating systems and carefully pinned first-party shared code provide interfaces and neutral primitives; they do not define InfiltratorFS semantics. External filesystems, papers and tools are evidence to compare against, not authorities that can silently change the meaning of the format.

The project does not equate newer with better. Proven ideas are retained when they remain the strongest design, and new mechanisms are accepted only when they improve correctness, crash behaviour, performance, resilience or explainability. A feature is not complete because it works once: its failure, recovery and qualification boundaries are part of the feature.

## What exists today

Format 0.18 provides:

- 4096-byte little-endian blocks and 128-bit filesystem/object identities;
- three physically separated checksummed checkpoints with generation-based recovery;
- copy-on-write transactions and retained historical generations;
- a sharded persistent allocation tree with rebuildable runtime free-extent indexes;
- generation-aware object and directory trees plus paged extent metadata;
- inline, sparse and shared/reflinked file data;
- symbolic links, hard links and named read-only snapshots;
- adaptive bounded per-extent compression using native IAC1 v1, with LZ4 retained as a non-default interoperability/reference representation;
- CRC64-ECMA metadata integrity and SHA-256 logical file-data integrity;
- 1023-byte UTF-8 namespace components;
- portable flags plus birth/access/modification/change timestamps stored as signed epoch seconds plus canonical nanoseconds, avoiding the old signed-64-bit nanosecond date ceiling;
- operating-system-specific metadata isolated at adapter boundaries; and
- fast structural filesystem checking plus explicit deep scrub, inspection and forensic tooling.

Linux is the most complete mounted adapter. The normal Linux path is the native out-of-tree `infiltratorfs.ko` VFS driver installed through DKMS; there is no current FUSE filesystem implementation or FUSE runtime fallback. The native driver includes the established read/write namespace surface, random and sparse I/O, truncate, `fallocate`, hole punching, FIEMAP/SEEK_DATA/SEEK_HOLE, reflinks, xattrs, special nodes, page cache/readahead, writable `mmap`, crash-safe open-unlink handling, checkpoint fallback/healing, online defragmentation, workload/media-aware allocation policy and online grow/bounded shrink.

Current development source includes native user/group/project quotas with mounted qualification. Resize is implemented and independently mounted-qualified. The authoritative feature status is `docs/ROADMAP.md`; exact evidence is `docs/QUALIFICATION.md`.

Windows currently provides native image/raw-partition access and a driverless Explorer bridge using Microsoft's inbox Projected File System (ProjFS). It is useful interoperability, but it is not a native InfiltratorFS Windows kernel driver. The native Windows filesystem driver remains future work.

## Linux quick start

Build on a Debian-family system with matching running-kernel headers:

```bash
sudo apt install build-essential cmake dkms kmod libssl-dev policykit-1 util-linux \
  xdg-utils libgtk-3-dev pkg-config python3 udev udisks2 \
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
./build/infilfs-forensic --jsonl infilfs.img
```

## Filesystem checking and scrub

The normal filesystem checker is intentionally fast and structural:

```bash
sudo fsck.infiltratorfs /dev/<partition>
```

It validates checkpoints, allocation-tree structure/accounting, the object index, namespace/reference consistency and checksum metadata. It does **not** reconstruct exact block ownership by walking every file/data block, read every user-data block, or recompute every payload checksum; those exhaustive checks belong to `--scrub`. On a freshly formatted empty filesystem it should complete essentially immediately.

The exhaustive data-integrity scan is opt-in:

```bash
sudo fsck.infiltratorfs --scrub /dev/<partition>
```

`--scrub` invokes the deep scrub path, which can read file payload data, recompute checksums and verify retained generations. It can therefore take a long time on a populated filesystem. There is no separate scrub executable: deep verification is intentionally a secondary mode of `fsck.infiltratorfs`. See `docs/FSCK-SCRUB-SEPARATION.md` for the command contract and rationale.

For mounted fragmentation metrics and bounded online defragmentation:

```bash
./build/infilfs-optimize --metrics /mnt/infiltratorfs/file.bin
./build/infilfs-optimize --defrag /mnt/infiltratorfs/file.bin
./build/infilfs-optimize --defrag --recursive /mnt/infiltratorfs/tree
```

The destructive physical qualification harness remains available as `tests/native-complete-qualification.sh`; it is an explicit operator test, not ordinary CI.

## Desktop and packaging

Linux packages include the native module/DKMS integration, `mkfs.infiltratorfs`, inspection/forensic tools, the unified `fsck.infiltratorfs` checker with optional `--scrub`, `mount.infiltratorfs`, the native C/GTK3 InfiltratorFS Manager, udev/UDisks identification and the repository's formatter-integration work for libblockdev/UDisks/GNOME Disks.

InfiltratorFS Manager formats and mounts selected non-system partitions through constrained privileged helpers. On Linux Mint, the project uses its own **Format partition as InfiltratorFS…** Nemo action rather than modifying Mintstick's whole-device formatting behaviour.

Published release assets are available from the repository's GitHub Releases page. Release publication performs its own installed-package native mount/scrub gate; milestone-scale million-file/1 TiB and endurance suites remain separate weekly/manual qualification rather than an automatic requirement for every release.

## Documentation ownership

The full documentation taxonomy and change discipline are defined in
`docs/README.md`. To prevent documentation drift, each kind of fact has one
authoritative home:

- `docs/ON_DISK_FORMAT.md` — persistent Format 0.18 layout and encoding contract.
- `docs/ARCHITECTURE.md` — design model and architectural invariants.
- `docs/ROADMAP.md` — **the only authoritative feature-completion list**.
- `docs/QUALIFICATION.md` — **the only authoritative exact-source qualification/evidence ledger**.
- `docs/PLATFORM_ADAPTERS.md` — operating-system adapter boundaries.
- `docs/SECURITY.md` — portable security/ACL design direction.
- `docs/COMPRESSION.md` — IAC1/compressed-extent design.
- `docs/FORENSICS.md` — forensic scanner model and use.
- `docs/FSCK-SCRUB-SEPARATION.md` — fast structural fsck versus explicit deep scrub contract.
- `docs/INSPIRATIONS.md` — historical design influences, not project status.

Implementation comments and workflow comments should explain local behaviour only; they are not alternate project-status documents.

## Development rule

Before 1.0, prefer the cleanest long-term filesystem design over preserving obsolete development-format assumptions. A feature is marked complete only when its implementation and required qualification are both complete.

## Licence

Copyright © 2016–2026 Shannon Smith.

InfiltratorFS is licensed under the GNU General Public License version 3 or, at your option, any later version (`GPL-3.0-or-later`).
