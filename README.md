<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# InfiltratorFS

[![Build and conformance](https://github.com/The-First-Infiltrator/InfiltratorFS/actions/workflows/ci.yml/badge.svg)](https://github.com/The-First-Infiltrator/InfiltratorFS/actions/workflows/ci.yml)

InfiltratorFS is a clean-sheet, platform-neutral general-purpose filesystem started in 2026. The persistent format and core engine are written in portable C; Linux and Windows are adapters over the same on-disk structures rather than separate filesystem implementations.

**Current implementation:** 0.16.13<br>
**On-disk format:** 0.12<br>
**Shared foundation:** Infiltratr Common 1.11.0<br>
**Licence:** GPL-3.0-or-later

Format 0.12 adds named read-only snapshot roots and retained historical generations. During pre-1.0 development, each build accepts only its current on-disk format; development media may need to be reformatted after a format revision.

## Capabilities

Format 0.12 currently provides:

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

Implementation 0.16.0 added native named snapshots. Each snapshot records a complete immutable generation root and can be listed, browsed, read, scrubbed and deleted through the portable API or direct-image tool. CoW reclamation preserves blocks referenced by any retained generation and releases them after the final reference is deleted. Implementation 0.16.1 removes pre-release backward-compatibility handling and accepts only current Format 0.12 media. Implementation 0.16.2 separates sequential data and metadata allocation directions so normal large copies remain compact instead of exhausting the bounded inline extent list. Implementation 0.16.3 completes the Linux userspace compatibility path needed by desktop file managers: persistent extended attributes, FIFO/Unix-socket/device-node metadata, normal fallocate, stable hard-link inode reporting, and nonzero inode-capacity statistics, with mounted regression coverage. Format 0.12 is unchanged. Implementation 0.16.4 fixes Linux-adapter metadata cleanup so probing an absent hidden metadata record cannot abort and silently roll back an ordinary unlink; mounted conformance now verifies both single-file and recursive tree deletion. Format 0.12 remains unchanged.

## Architecture

| Layer | Status |
| --- | --- |
| On-disk format | Platform-neutral development Format 0.12 only. |
| Core filesystem engine | Portable C17; no Linux fd/VFS or Win32 handle types. |
| Shared foundations | Infiltratr Common 1.11.0. |
| Storage interface | Callback-based POSIX, Win32 and in-memory backends. |
| Linux userspace adapter | Implemented through POSIX I/O and FUSE3. |
| Native Linux kernel driver | Read-only VFS implementation; packaged through DKMS since 0.16.10. |
| Windows transfer/raw-volume adapter | Implemented for direct current-Format partition access. |
| Native Windows filesystem driver / Explorer mount | Future work. |

Filesystem-specific transaction, allocation, namespace, checksum and on-disk-format rules remain in InfiltratorFS. Common owns only generally reusable primitives such as endian conversion, UTF-8 validation, exact POSIX positioned I/O and checked arithmetic.

The Windows transfer application is not a Windows kernel filesystem driver. It can discover and access supported InfiltratorFS partitions directly, including partitions without a drive letter, but Explorer drive-letter mounting remains future work.

## Build and test

On Linux Mint, Ubuntu or another supported Debian-family development host:

```bash
sudo apt install build-essential cmake pkg-config libfuse3-dev fuse3 dkms kmod linux-headers-$(uname -r)
git clone --recurse-submodules https://github.com/The-First-Infiltrator/InfiltratorFS.git
cd InfiltratorFS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
make -C kernel
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

The traditional `mount.infiltratorfs` helper remains on the mature FUSE path during native-kernel bring-up. To exercise the packaged native read-only module after installation, bypass the helper with util-linux `mount -i`:

```bash
sudo modprobe infiltratorfs
sudo mount -i -t infiltratorfs -o ro /dev/loop0 /mnt/infiltratorfs
fsck.infiltratorfs -n infilfs.img
```

GitHub Actions runs Linux, Clang, sanitizer and static-analyzer suites, cross-platform image qualification, native Windows builds/tests, native Linux kernel-module compilation and Linux package construction. The native module workflow also reproduces the DKMS source-root build with `KERNELRELEASE` already exported, matching the packaging environment that exposed the 0.16.11 installation failure.

## Desktop and Windows tools

The Linux Debian package installs **InfiltratorFS Manager**, which can create/format images, select non-system disk partitions including fixed, USB, SD and other removable media, inspect, scrub, run forensic raw-metadata scans, mount through FUSE, open in Nemo and unmount safely. Whole disks and storage backing the active system partitions remain excluded. The same package now installs the read-only native Linux module as DKMS source, so the host builds `infiltratorfs.ko` against its own installed kernel headers rather than receiving a kernel-specific binary.

The Windows release contains a versioned executable such as `InfiltratorFS-Windows-0.16.13.exe`. Run it elevated when accessing raw media. It can discover physical partitions, list the root directory, copy files/folders and run a full scrub while bounding raw I/O to the selected partition.

## Release assets

A numbered release publishes:

| File | Purpose |
| --- | --- |
| `infiltratorfs_<version>_amd64.deb` | Generic Linux amd64 Debian package, including DKMS source for the native read-only kernel module. |
| `infiltratorfs-<version>-linux-native.run` | Native local Linux build/install program, including DKMS module installation. |
| `InfiltratorFS-Windows-<version>.exe` | Native Windows transfer/raw-volume application. |
| `SHA256SUMS.txt` | SHA-256 checksums for all published project artifacts. |

GitHub provides the standard source-code ZIP and tarball automatically from the exact release tag.

To inspect the native Linux build before allowing it to install anything:

```bash
chmod +x infiltratorfs-0.16.13-linux-native.run
./infiltratorfs-0.16.13-linux-native.run --dry-run
```

Run the same file without `--dry-run` to compile, test and install InfiltratorFS natively. If required packages or the running kernel's headers are missing, it displays the exact `apt-get` commands and asks before installing them.

## Repository and release policy

This repository uses `main` as its working branch. Development changes are made directly on `main`; the normal project workflow does not depend on PR, feature or release branches.

Every push to `main` runs Build and conformance. Ordinary commits do not publish. A commit is release-eligible only when its subject begins with the exact implementation version as `Release <version>` and the complete Build and conformance workflow succeeds.

The publisher checks out the exact tested commit, verifies it is still current `main`, rebuilds/tests Linux and Windows release targets, constructs the `.deb`, `.run` and Windows executable, generates checksums, then creates the version tag and GitHub release. GitHub supplies source-code archives from that tag automatically. Existing version tags and published releases are immutable and are never moved or replaced.

Manually runnable build/test helpers, where present, are diagnostic tools only and are not release-approval mechanisms.

## Recovery and safety

A healthy block-device open validates the checkpoints, bitmap and essential root/index state needed for operation without walking every file. Full namespace/ownership/checksum graph verification belongs to **Scrub / Verify**.

If a writable open sees missing or disagreeing checkpoint replicas, each candidate is fully graph-validated before any healing occurs. A corrupt newer graph may fall back to an older valid committed generation; an unreadable checkpoint location remains a hard writable-open error because it might contain the only durable newer generation.

InfiltratorFS remains experimental. Keep verified backups and use disposable/test media during development. The FUSE adapter remains the authoritative writable Linux path while the native kernel adapter is intentionally read-only and is brought up incrementally against the same Format 0.12 media.

## Repository layout

```text
include/infilfs/          public format/storage/filesystem interfaces
src/                      portable filesystem core
src/infiltratr-common/    pinned Infiltratr Common submodule
src/platform/             POSIX and Win32 storage adapters
kernel/                   native Linux read-only VFS adapter and DKMS source
fuse/                     Linux FUSE3 adapter
tools/                    formatter, inspector, scrubber and transfer tools
tests/                    conformance, recovery, crash and platform tests
docs/                     architecture and on-disk-format documentation
```

## Licence

InfiltratorFS is free software licensed under the GNU General Public License version 3 or, at your option, any later version (`GPL-3.0-or-later`). See the repository licence file for the complete terms.


Implementation 0.16.5 removes sequential-write amplification: aligned hole/new-file writes use contiguous full-overwrite runs, transaction bitmap images skip redundant pre-zeroing, and Linux FUSE automatic publication scales from 16 MiB to a bounded 256 MiB while explicit fsync/sync remains immediate. Format 0.12 is unchanged.


Implementation 0.16.6 removes sequential-write CPU amplification: checksum-object lookup keeps a validated runtime cursor so forward growth is linear instead of restarting from the checksum-chain head, and CRC64-ECMA metadata checks use an on-disk-identical table-driven implementation instead of bit-at-a-time processing. Format 0.12 is unchanged.


Implementation 0.16.7 / Format 0.12 adds operation-level transaction savepoints, runtime hashed object/directory indexes, and checksummed paged extent metadata so fragmented files are no longer bounded by the classic single-object extent array.


Implementation 0.16.8 adds snapshot-coordinated online scrub. The core can scrub an explicitly named immutable generation or automatically pin the current committed generation with a temporary retained snapshot, verify that stable view, report the exact generation checked, and remove the temporary retention record afterward. Format 0.12 is unchanged.


Implementation 0.16.9 hardens scattered writes to paged-extent files. Non-tail random overwrites now rebuild the complete ordered extent-page set with copy-on-write publication instead of relying on the former single-page replacement path. Conformance includes a fresh 512 MiB sparse file with deterministic scattered 4 KiB writes, durability sync, scrub, continued writes and reopen verification. Format 0.12 is unchanged.


Implementation 0.16.10 introduces the first packaged native Linux VFS path. The out-of-tree `infiltratorfs.ko` driver can mount current Format 0.12 block devices read-only, traverse classic/paged indexes and directories, preserve hard-link inode identity, expose symlinks, and read inline, sparse, classic-extent and paged-extent files. Linux packages install the module source through DKMS. Native mounts are exercised explicitly with `mount -i` while the standard mount helper remains on the mature FUSE path during bring-up. Format 0.12 is unchanged.

Implementation 0.16.11 ports the packaged native Linux read-only module to the current fs_context/get_tree_bdev VFS mount API used by Linux 7.0, relies on the VFS default inode-drop policy for compatibility across supported kernel generations, and makes a DKMS build failure non-fatal to installation of the userspace/FUSE tools. Format 0.12 is unchanged.

Implementation 0.16.12 fixes the packaged DKMS build entry. DKMS exports `KERNELRELEASE` before its initial make, and the previous conditional Makefile therefore exposed no top-level target and terminated with `make: *** No targets. Stop.`. The kernel Makefile now keeps its external-module build targets available in that environment, and CI permanently reproduces that exact DKMS-style source-root invocation. Format 0.12 is unchanged.

Implementation 0.16.13 adds the Linux 7.0 inode-state compatibility path. Linux 7.0 changed `inode->i_state` from a directly bit-testable scalar to `struct inode_state_flags`; the native adapter now uses the kernel `inode_state_read_once()` accessor on 7.0+ while preserving the legacy read path on earlier supported kernels. Format 0.12 is unchanged.
