<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# InfiltratorFS

[![Build and conformance](https://github.com/Infiltrator-Projects/InfiltratorFS/actions/workflows/ci.yml/badge.svg)](https://github.com/Infiltrator-Projects/InfiltratorFS/actions/workflows/ci.yml)

InfiltratorFS is a clean-sheet, platform-neutral general-purpose filesystem started in 2026. The persistent format and portable core define the filesystem; operating-system adapters translate native APIs and semantics onto the same on-disk objects, transactions, extents, snapshots and integrity model.

**Current release:** 0.18.30 (Format 0.17)<br>
**Shared foundation:** Infiltratr Common 1.11.0  
**Licence:** GPL-3.0-or-later

Pre-1.0 development intentionally does not preserve compatibility with older development formats. Test media may need to be reformatted after an on-disk-format revision.

## What exists today

Format 0.17 provides 4096-byte little-endian blocks, 128-bit filesystem/object identities, three physically separated checksummed checkpoints, authoritative one-bit-per-block allocation persisted as a sharded CoW allocation tree with rebuildable runtime free-extent indexes, 1023-byte UTF-8 namespace components, copy-on-write transactions, a generation-aware object-index radix tree, hashed directory trees, paged extent metadata, inline small files, sparse files and hole extents, shared extents/reflinks, symbolic and hard links, retained historical generations and named read-only snapshots, CRC64-ECMA metadata integrity, SHA-256 file-data integrity, portable attributes, isolated POSIX compatibility metadata, UTF-8 namespace validation, scrub/verify and callback-based storage/durability/randomness/clock services.

Linux is the most complete mounted adapter. Release 0.18.30 uses the native `infiltratorfs.ko` VFS driver and includes native namespace mutation, random and sparse writes, truncate, `fallocate`, hole punching, hard links and symlinks, crash-safe open-unlink lifetime, mount-time orphan recovery, persistent standard Linux xattr namespaces, FIFO/socket/character/block special-node identity, page-cache and readahead integration, shared writable `mmap`, snapshot-preserving live writes, checkpoint fallback and replica healing, complete allocation reporting, bounded metadata-tree alias/cycle rejection, kernel-stack-safe allocation-tree traversal, online defragmentation and native remount/scrub qualification. A 64-shard volatile reservation layer lets independent writers search and reserve free data runs before the serialized metadata transaction, while per-object placement cursors preserve sequential locality. Sustained native writes use a writer-tail checksum path and persistent tree indexes so multi-gigabyte sequential appends no longer rescan historical checksum/index metadata. Mounted CI also exercises the former FUSE regression surface, including six-way concurrent namespace mutation, shared-inode writes/fsync, same-inode xattr readers/writers, open-unlink contention, 4,000 random overwrites and repeated durability publication.

Windows release 0.18.30 includes a driverless Explorer bridge on top of Microsoft's inbox Projected File System (ProjFS). The Windows application uses a resizable native manager layout with a storage selector, selected-volume identity, a report-style root-directory view, grouped maintenance/transfer actions and system-DPI-aware themed controls. It opens an InfiltratorFS image or raw partition through the portable core, **Mount in Explorer** assigns a temporary drive letter, Linux-created files are hydrated on demand, and ordinary Windows create/write/rename/hard-link/delete operations are committed back to the same Format 0.17 volume. This interim path ships no InfiltratorFS kernel driver; the true native Windows filesystem driver remains future work.

## Platform-neutral architecture

InfiltratorFS does not treat Linux semantics as the filesystem definition and does not intend Windows, macOS, BSD, Haiku or another operating system to be compatibility layers over Linux. The common model lives below all adapters.

When operating systems expose the same underlying concept under different names, adapters map that concept to one InfiltratorFS representation. When an operating system has genuinely additional semantics, those semantics are preserved without forcing another adapter to invent support for them or destroy metadata it does not understand.

See [`docs/PLATFORM_ADAPTERS.md`](docs/PLATFORM_ADAPTERS.md) for the adapter contract and [`docs/SECURITY.md`](docs/SECURITY.md) for the planned cross-platform identity/ACL model.

## Linux architecture

Linux mounting is performed solely by the native `infiltratorfs.ko` VFS filesystem driver. The Debian package and native `.run` installer install it through DKMS and load it with `modprobe infiltratorfs`.

Normal Linux mounts are kernel mounts:

```bash
sudo modprobe infiltratorfs
sudo mount -t infiltratorfs -o rw /dev/<partition> /mnt/infiltratorfs
findmnt -T /mnt/infiltratorfs -o SOURCE,FSTYPE,OPTIONS
```

`FSTYPE` must be `infiltratorfs`. The current Linux source tree, package and installer contain no FUSE filesystem implementation, no `infilfs-fuse` executable and no FUSE runtime dependency. The old FUSE adapter exists only in Git history.

The standard `mount.infiltratorfs` helper, InfiltratorFS Manager, Nemo/UDisks integration and direct `mount -t infiltratorfs` all target the native kernel filesystem. The helper calls util-linux `mount -i` internally so it cannot recurse back into itself.

## Native durability and integrity

The native writer uses Format 0.17 transactions, range-journal rollback and extent-backed data. Allocation commits rewrite only affected allocation leaves and the corresponding CoW branch paths plus the replacement root, rather than copying or rewriting the full volume bitmap. Native reads validate metadata CRC64 and file SHA-256 data checksums. `fsync`, `syncfs` and global `sync` publish pending transactions through the normal durability boundary.

The dedicated native-kernel workflow builds the out-of-tree module, reproduces the DKMS source-root build and performs mounted native qualification. Corruption qualification includes checksummed aliased metadata graphs that must fail quickly with `EFSCORRUPTED`/`EUCLEAN` rather than permit unbounded repeated traversal. Native directory/object-index readers are serialized with deferred metadata publication so reclaimed topology cannot be observed mid-walk, and readdir snapshots persistent entries before Linux sidecar type resolution so the writer lock is never recursively acquired. Package upgrades also respect an administrator's explicit `modprobe` disable policy so an emergency blacklist can remain in force while a fixed DKMS module is installed. The mounted suite covers namespace operations, sequential/random/sparse writes, high-offset sparse files, truncate, allocation reporting, `fallocate`, hole punching, xattrs, special nodes, page-cache/readahead/mmap behavior, open-unlink lifetime, snapshot-preserving writes, bounded near-full fragmentation/refill allocation, remount readback and offline scrub.

Release publication adds an installed-package gate: it installs the generated `.deb`, verifies native filesystem registration, mounts a real Format 0.17 loop image with `FSTYPE=infiltratorfs`, writes and byte-compares non-zero data, syncs, unmounts and requires a clean userspace scrub. Publication also rejects any FUSE executable, process or package dependency.

A full checked-roadmap audit was completed on 2026-08-29 against source commit `075aed9c737fb38cc408d752736a97773dc2a035`: all 56 checked roadmap entries retained current evidence, the destructive physical native-VFS harness passed 69/69 checks, four additional mounted concurrency rounds passed, and the final generation-3695 scrub checked 1,446 files and 151,580 data blocks with zero checksum or metadata errors. Detailed evidence and performance telemetry are recorded in [docs/QUALIFICATION.md](docs/QUALIFICATION.md).

## Linux desktop integration

The Debian package installs **InfiltratorFS Manager** plus udev/UDisks identification rules. Manager is a persistent GTK desktop application with automatic non-system partition discovery, selected-volume identity and mount state, themed destructive-action handling, a built-in operation console and non-blocking maintenance jobs. It can create/format images, inspect, scrub, run forensic scans, mount natively, open in the desktop file manager and unmount safely.

For block devices Manager uses a native kernel mount under `/media/<user>/InfiltratorFS`. For image files it uses a kernel loop mount under `~/InfiltratorFS`. The privileged helper verifies that the resulting filesystem type is exactly `infiltratorfs` and refuses a non-native mount.

The source tree includes the conventional `mkfs.infiltratorfs` helper and
version-pinned integration patches for libblockdev, UDisks and GNOME Disks.
Continuous integration applies those patches to the pinned upstream projects,
builds the complete stack, invokes libblockdev's generic formatter with a
label, and verifies the resulting Format 0.17 image through
`infilfs-inspect --udev`. Distribution-provided GNOME Disks packages will not
show the new format row until their maintainers adopt the patches; already
formatted volumes remain identifiable and mountable through the installed
desktop integration.

## Build and test on Linux

On Linux Mint, Ubuntu or another supported Debian-family host:

```bash
sudo apt install build-essential cmake dkms kmod policykit-1 util-linux \
  xdg-utils python3 python3-gi gir1.2-gtk-3.0 udev udisks2 linux-headers-$(uname -r)

git clone --recurse-submodules https://github.com/Infiltrator-Projects/InfiltratorFS.git
cd InfiltratorFS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
make -C kernel KDIR=/lib/modules/$(uname -r)/build
```

For the complete destructive regression and performance qualification against
the dedicated partition 22 test device, use an exact release tag and run:

```bash
bash tests/native-complete-qualification.sh
```

The script requires an explicit typed confirmation before erasing
`/dev/mmcblk0p22`. It tests the installed release, all portable tests and policy
guards, native packages, the exact running-kernel module, the full physical VFS
surface, online defragmentation, one million files, a 1 TiB sparse volume,
near-full mixed-workload endurance, remount persistence, scrubs and conservative
performance-regression floors. A complete run can take several hours and needs
at least 20 GiB free in its temporary filesystem.

Create and inspect a regular image without mounting:

```bash
truncate -s 128M infilfs.img
./build/mkfs.infilfs -L test-volume infilfs.img
./build/infilfs-inspect infilfs.img
./build/infilfs-scrub infilfs.img
./build/infilfs-forensic --jsonl infilfs.img
```

For a mounted native filesystem, `infilfs-optimize` reports per-file extent
fragmentation and can perform bounded copy-on-write online defragmentation
without replacing the inode:

```bash
./build/infilfs-optimize --metrics /media/user/InfiltratorFS/file.bin
./build/infilfs-optimize --defrag /media/user/InfiltratorFS/file.bin
./build/infilfs-optimize --defrag --recursive /media/user/InfiltratorFS/tree
```

The direct-image tool can exercise namespace and snapshot operations without mounting:

```bash
./build/infilfs-tool infilfs.img mkdir /docs
./build/infilfs-tool infilfs.img put README.md /docs/README.md
./build/infilfs-tool infilfs.img reflink /docs/README.md /docs/README-copy.md
./build/infilfs-tool infilfs.img symlink README.md /docs/current
./build/infilfs-tool infilfs.img link /docs/README.md /docs/README-hardlink.md
./build/infilfs-tool infilfs.img snapshot-create before-edit
./build/infilfs-tool infilfs.img snapshot-list
./build/infilfs-tool infilfs.img snapshot-cat before-edit /docs/README.md
```

## Native installer

A release includes `infiltratorfs-<version>-linux-native.run`. It contains the tested source tree, checks host requirements, builds/tests userspace code, installs the DKMS source, builds the module for the running kernel, loads it, verifies `/proc/filesystems`, refreshes desktop storage rules and removes any obsolete `/usr/bin/infilfs-fuse` left by an older unmanaged installation.

For the current release:

```bash
chmod +x infiltratorfs-0.18.30-linux-native.run
./infiltratorfs-0.18.30-linux-native.run --dry-run
```

The installer refuses to upgrade while an InfiltratorFS volume is mounted. This prevents replacing a live filesystem driver underneath active media.

## Release assets

A numbered release publishes only the project-built artifacts below; GitHub supplies its standard source archives automatically.

| File | Purpose |
| --- | --- |
| `infiltratorfs_<version>_amd64.deb` | Native Linux amd64 package with self-contained DKMS source. |
| `infiltratorfs-<version>-linux-native.run` | Native local Linux compile/test/install program. |
| `InfiltratorFS-Windows-<version>.exe` | Windows direct-transfer/raw-volume application. |
| `SHA256SUMS.txt` | SHA-256 checksums for all project-built release artifacts. |

The Linux package has no FUSE runtime dependency. Its `postinst` requires matching running-kernel headers and treats a DKMS build/load failure as an installation failure rather than silently falling back to userspace mounting.

## Recovery and safety

A healthy open validates checkpoint candidates, allocation and the essential committed graph. Full namespace, ownership, checksum and metadata-graph validation belongs to **Scrub / Verify**. Writable recovery only heals from a graph-valid committed generation and fails closed where durable ordering cannot be established safely.

InfiltratorFS remains experimental and pre-1.0. Keep verified backups and use disposable/test media for development qualification.

## Repository and release policy

Development uses `main` only. Ordinary pushes run CI but do not publish. A release-eligible commit subject begins with `Release <version>`. After Build and conformance succeeds, the publisher verifies that the exact commit is still current `main`, rebuilds Linux and Windows assets, installs and natively mounts the Linux `.deb`, validates the asset set and checksums, creates the exact tag and publishes the release.

## Repository layout

```text
include/infilfs/          public format/storage/filesystem interfaces
src/                      portable filesystem core
src/infiltratr-common/    pinned Infiltratr Common submodule
src/platform/             POSIX and Win32 storage adapters
kernel/                   native Linux VFS adapter and DKMS source
tools/                    formatter, inspector, scrubber, manager and transfer tools
tests/                    conformance, recovery, crash and platform tests
docs/                     architecture, format, security and adapter documentation
```

## Licence

InfiltratorFS is free software licensed under the GNU General Public License version 3 or, at your option, any later version (`GPL-3.0-or-later`).

## Driverless Windows Explorer bridge

The Windows transfer application can expose an opened InfiltratorFS volume as a temporary drive letter using Microsoft's inbox Projected File System (ProjFS). **Mount in Explorer** hydrates files from the portable core on demand; existing Linux-created files can be edited in Windows, and Windows file create/write/delete/rename/hard-link operations are committed back to InfiltratorFS when handles close. This mode does not ship an InfiltratorFS kernel driver and therefore does not require InfiltratorFS driver signing; Windows' optional Microsoft-signed ProjFS component must be enabled.
