<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Native Linux kernel module

This directory contains the native Linux VFS adapter for InfiltratorFS. It is an out-of-tree kernel module: Linux itself does not need to be rebuilt. The module is built against installed kernel headers. Release 0.16.11 packages the source through DKMS so normal kernel upgrades rebuild only `infiltratorfs.ko` for kernels whose matching headers are installed.

The current milestone is a genuine read-only filesystem implementation rather than the earlier empty-root bootstrap. The module now:

- registers the `infiltratorfs` filesystem type with Linux VFS;
- mounts an InfiltratorFS block device read-only through `mount_bdev`;
- selects the highest-generation structurally valid checkpoint from the three physical checkpoint locations;
- requires current Format 0.12 and rejects unknown incompatible feature bits;
- opens the real persistent root object;
- resolves object IDs through both classic and paged object indexes;
- enumerates and looks up entries from classic and paged directories;
- creates stable Linux inodes from persistent InfiltratorFS object IDs, preserving regular-file hard-link identity;
- exposes regular files, directories and symbolic links through native VFS objects;
- reads inline files, ordinary extents, sparse holes and paged extents directly from the block device; and
- remains strictly read-only, rejecting writable mounts.

Build against the running kernel when matching headers are installed:

```bash
make -C kernel
```

Or build against an explicit kernel-header tree:

```bash
make -C kernel KDIR=/usr/src/linux-headers-$(uname -r)
```

The result is `kernel/infiltratorfs.ko`. Loading it is a normal module operation (`insmod`/`modprobe`), not a kernel rebuild.

The Debian package installs the module sources under `/usr/src/infiltratorfs-<version>` and registers/builds them through DKMS. The native `.run` installer likewise installs the tested source through DKMS after completing the userspace build. If the running kernel's headers are unavailable, install `linux-headers-$(uname -r)` and run `sudo dkms autoinstall`.

The traditional `mount.infiltratorfs` helper deliberately remains on the mature FUSE path during this bring-up. To exercise the native read-only module without invoking the helper recursively, load it with `modprobe infiltratorfs` and use `mount -i -t infiltratorfs -o ro <block-device> <mountpoint>`.

The dedicated kernel-module GitHub Actions workflow always compiles the module against Ubuntu generic kernel headers and checks its module metadata. When the hosted runner also exposes headers matching its running kernel, the workflow additionally builds the userspace formatter/tools, creates a real Format 0.12 image containing directories, an inline file, an extent-backed file and a symbolic link, loads `infiltratorfs.ko`, mounts the image through a loop block device, and byte-compares files through the native kernel mount.

## Deliberate limitations of this milestone

This adapter is not yet a replacement for FUSE in normal writable use. The userspace portable core remains the authoritative, fully validated implementation while the kernel path is brought up incrementally.

Still pending are:

- CRC64 verification of checkpoints, objects and metadata pages in the kernel path;
- SHA-256 verification of file-data blocks and checksum-chain traversal;
- full graph/ownership validation and recovery selection equivalent to the portable core;
- Linux uid/gid/time/xattr mapping beyond the basic read-only inode metadata;
- page-cache, readahead and mmap integration;
- all writable VFS operations and transaction publication; and
- concurrency/locking hardening.

The first target is a trustworthy native read-only mount of the same Format 0.12 media used by FUSE and the Windows adapter. FUSE remains the comparison/reference Linux adapter until the native path reaches equivalent correctness coverage.
