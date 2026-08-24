<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# InfiltratorFS

[![Build and conformance](https://github.com/The-First-Infiltrator/InfiltratorFS/actions/workflows/ci.yml/badge.svg)](https://github.com/The-First-Infiltrator/InfiltratorFS/actions/workflows/ci.yml)

InfiltratorFS is a clean-sheet, platform-neutral general-purpose filesystem started in 2026. The persistent format and core engine are written in portable C; Linux and Windows are adapters over the same on-disk structures rather than separate filesystem implementations.

**Current implementation:** 0.16.3<br>
**On-disk format:** 0.11<br>
**Shared foundation:** Infiltratr Common 1.11.0<br>
**Licence:** GPL-3.0-or-later

Format 0.11 adds named read-only snapshot roots and retained historical generations. During pre-1.0 development, each build accepts only its current on-disk format; development media may need to be reformatted after a format revision.

## Capabilities

Format 0.11 currently provides:

- 4096-byte little-endian blocks;
- nonzero 128-bit persistent filesystem and object identities;
- three physically separated checksummed checkpoints with generation-based recovery;
- authoritative bitmap allocation with opposing data/metadata next-fit hints;
- transactional copy-on-write metadata/data publication;
- paged directory and object-index metadata;
- inline small files with automatic inline/extent transitions;
- normal/hole extents, sparse growth and hole punching;
- shared extents/reflinks with CoW on later writes;
- first-class relative and absolute UTF-8 symbolic links;
- regular-file hard links with shared persistent identity;
- named read-only snapshots with immutable root, index and bitmap images;
- transactional retention and reclamation of historical metadata and data;
- CRC64-ECMA metadata integrity and SHA-256 file-data integrity;
- portable timestamps/attributes plus optional POSIX compatibility metadata;
- mandatory well-formed UTF-8 namespace components;
- namespace, ownership, checksum-chain and metadata-graph validation;
- explicit read-only scrub/verify; and
- callback-based storage, durability, randomness and clock services.

Implementation 0.16.0 added native named snapshots. Each snapshot records a complete immutable generation root and can be listed, browsed, read, scrubbed and deleted through the portable API or direct-image tool. CoW reclamation preserves blocks referenced by any retained generation and releases them after the final reference is deleted. Implementation 0.16.1 removes pre-release backward-compatibility handling and accepts only current Format 0.11 media. Implementation 0.16.2 separates sequential data and metadata allocation directions so normal large copies remain compact instead of exhausting the bounded inline extent list. Implementation 0.16.3 completes the Linux userspace compatibility path needed by desktop file managers: persistent extended attributes, FIFO/Unix-socket/device-node metadata, normal fallocate, stable hard-link inode reporting, and nonzero inode-capacity statistics, with mounted regression coverage. Format 0.11 is unchanged.

## Architecture

| Layer | Status |
| --- | --- |
| On-disk format | Platform-neutral development Format 0.11 only. |
| Core filesystem engine | Portable C17; no Linux fd/VFS or Win32 handle types. |
| Shared foundations | Infiltratr Common 1.11.0. |
| Storage interface | Callback-based POSIX, Win32 and in-memory backends. |
| Linux userspace adapter | Implemented through POSIX I/O and FUSE3. |
| Windows transfer/raw-volume adapter | Implemented for direct current-Format partition access. |
| Native Linux kernel driver | Future work. |
| Native Windows filesystem driver / Explorer mount | Future work. |

Filesystem-specific transaction, allocation, namespace, checksum and on-disk-format rules remain in InfiltratorFS. Common owns only generally reusable primitives such as endian conversion, UTF-8 validation, exact POSIX positioned I/O and checked arithmetic.

The Windows transfer application is not a Windows kernel filesystem driver. It can discover and access supported InfiltratorFS partitions directly, including partitions without a drive letter, but Explorer drive-letter mounting remains future work.

## Build and test

On Linux Mint, Ubuntu or another supported Debian-family development host:

```bash
sudo apt install build-essential cmake pkg-config libfuse3-dev fuse3
git clone --recurse-submodules https://github.com/The-First-Infiltrator/InfiltratorFS.git
cd InfiltratorFS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Create and inspect an image:

```bash
truncate -s 128M infilfs.img
./build/mkfs.infilfs -L test-volume infilfs.img
./build/infilfs-inspect infilfs.img
./build/infilfs-scrub infilfs.img
./build/infilfs-forensic --jsonl infilfs.img
```

Exercise the filesystem without mounting:

```bash
./build/infilfs-tool infilfs.img mkdir /docs
./build/infilfs-tool infilfs.img put README.md /docs/README.md
./build/infilfs-tool infilfs.img reflink /docs/README.md /docs/README-copy.md
./build/infilfs-tool infilfs.img symlink README.md /docs/current
./build/infilfs-tool infilfs.img readlink /docs/current
./build/infilfs-tool infilfs.img link /docs/README.md /docs/README-hardlink.md
./build/infilfs-tool infilfs.img snapshot-create before-edit
./build/infilfs-tool infilfs.img snapshot-list
./build/infilfs-tool infilfs.img snapshot-cat before-edit /docs/README.md
./build/infilfs-tool infilfs.img cat /docs/README.md
```

Mount through FUSE3:

```bash
mkdir -p /tmp/infilfs-mnt
./build/infilfs-fuse infilfs.img /tmp/infilfs-mnt -f
```

After installation, use the standard Linux helpers:

```bash
mount -t infiltratorfs infilfs.img /mnt/infiltratorfs -o rw
fsck.infiltratorfs -n infilfs.img
```

GitHub Actions runs Linux, Clang, sanitizer and static-analyzer suites, cross-platform image qualification, native Windows builds/tests and Linux package construction.

## Desktop and Windows tools

The Linux Debian package installs **InfiltratorFS Manager**, which can create/format images, select non-system disk partitions including fixed, USB, SD and other removable media, inspect, scrub, run forensic raw-metadata scans, mount through FUSE, open in Nemo and unmount safely. Whole disks and storage backing the active system partitions remain excluded.

The Windows release contains a versioned executable such as `InfiltratorFS-Windows-0.16.3.exe`. Run it elevated when accessing raw media. It can discover physical partitions, list the root directory, copy files/folders and run a full scrub while bounding raw I/O to the selected partition.

## Release assets

A numbered release publishes:

| File | Purpose |
| --- | --- |
| `infiltratorfs_<version>_amd64.deb` | Generic Linux amd64 Debian package. |
| `infiltratorfs-<version>-linux-native.run` | Native local Linux build/install program. |
| `InfiltratorFS-Windows-<version>.exe` | Native Windows transfer/raw-volume application. |
| `SHA256SUMS.txt` | SHA-256 checksums for all published project artifacts. |

GitHub provides the standard source-code ZIP and tarball automatically from the exact release tag.

To inspect the native Linux build before allowing it to install anything:

```bash
chmod +x infiltratorfs-0.16.3-linux-native.run
./infiltratorfs-0.16.3-linux-native.run --dry-run
```

Run the same file without `--dry-run` to compile, test and install InfiltratorFS natively. If required packages are missing, it displays the exact `apt-get` commands and asks before installing them.

## Repository and release policy

This repository uses `main` as its working branch. Development changes are made directly on `main`; the normal project workflow does not depend on PR, feature or release branches.

Every push to `main` runs Build and conformance. Ordinary commits do not publish. A commit is release-eligible only when its subject begins with the exact implementation version as `Release <version>` and the complete Build and conformance workflow succeeds.

The publisher checks out the exact tested commit, verifies it is still current `main`, rebuilds/tests Linux and Windows release targets, constructs the `.deb`, `.run` and Windows executable, generates checksums, then creates the version tag and GitHub release. GitHub supplies source-code archives from that tag automatically. Existing version tags and published releases are immutable and are never moved or replaced.

Manually runnable build/test helpers, where present, are diagnostic tools only and are not release-approval mechanisms.

## Recovery and safety

A healthy block-device open validates the checkpoints, bitmap and essential root/index state needed for operation without walking every file. Full namespace/ownership/checksum graph verification belongs to **Scrub / Verify**.

If a writable open sees missing or disagreeing checkpoint replicas, each candidate is fully graph-validated before any healing occurs. A corrupt newer graph may fall back to an older valid committed generation; an unreadable checkpoint location remains a hard writable-open error because it might contain the only durable newer generation.

InfiltratorFS remains experimental. Keep verified backups and use disposable/test media during development. The current FUSE adapter is deliberately single-threaded while the core remains single-writer. Persistent file-handle lifetime across unlink and rename is implemented in the adapter; parallel operation and the final native kernel-filesystem model remain future work.

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

## Licence

InfiltratorFS is free software licensed under the GNU General Public License version 3 or, at your option, any later version (`GPL-3.0-or-later`). See the repository licence file for the complete terms.
