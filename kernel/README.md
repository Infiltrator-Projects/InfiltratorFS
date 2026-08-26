<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Native Linux kernel module

This directory contains the native Linux VFS adapter for InfiltratorFS. It is an out-of-tree kernel module built against installed kernel headers and packaged through DKMS; Linux itself does not need to be rebuilt.

Implementation 0.18.3 is the normal Linux filesystem path. The old userspace FUSE adapter has been removed from the source tree and survives only in Git history.

The module:

- registers the `infiltratorfs` filesystem type with Linux VFS;
- mounts current Format 0.12 block devices read-only or read-write;
- selects the highest-generation structurally valid checkpoint from the three physical checkpoint locations;
- requires current Format 0.12 and rejects unknown incompatible feature bits;
- resolves classic and paged object indexes and directories;
- creates stable Linux inode identities from persistent InfiltratorFS object IDs;
- exposes regular files, directories and symbolic links through native VFS objects;
- reads inline data, ordinary extents, sparse holes and paged extents directly from the block device;
- performs native create, mkdir, write, setattr and durability publication operations through the Format 0.12 transaction path;
- supports extent-backed native writes rather than the earlier inline-only bring-up path;
- validates metadata integrity and file-data checksums on native reads; and
- publishes pending transactions at `fsync`, `syncfs` and global sync durability boundaries.

Build against the running kernel when matching headers are installed:

```bash
make -C kernel KDIR=/lib/modules/$(uname -r)/build
```

The result is `kernel/infiltratorfs.ko`. Loading it is a normal module operation:

```bash
sudo modprobe infiltratorfs
```

A direct native mount is then:

```bash
sudo mount -i -t infiltratorfs -o rw /dev/<partition> /mnt/infiltratorfs
findmnt -T /mnt/infiltratorfs -o SOURCE,FSTYPE,OPTIONS
```

`FSTYPE` must report `infiltratorfs`.

The Debian package installs the module source under `/usr/src/infiltratorfs-<version>` and registers/builds it through DKMS. The native `.run` installer does the same after its userspace build and conformance tests. Matching headers for the running kernel are mandatory; installation fails rather than falling back to another filesystem implementation.

The DKMS source root is deliberately self-contained and includes:

- `Makefile`
- `infiltratorfs.c`
- `infiltratorfs_format.h`
- `infiltratorfs_rw.inc`
- `infiltratorfs_rw_legacy.inc`
- `infiltratorfs_rw_data.inc`
- `infiltratorfs_rw_namespace.inc`
- `infiltratorfs_rw_read_cache.inc`
- `infiltratorfs_pagecache.inc`
- `infiltratorfs_linux_meta.inc`

The dedicated `Native Linux kernel module` GitHub Actions workflow compiles the module against Ubuntu kernel headers, reproduces the DKMS source-root build, checks module metadata and, when matching running-kernel headers are available, loads the module and performs a real loop-device native read/write transaction followed by userspace scrub qualification.

The release publisher adds an installed-package gate: it installs the generated `.deb`, verifies `/proc/filesystems` and `modinfo`, mounts a real Format 0.12 image as `infiltratorfs`, writes and byte-compares non-zero data, syncs, unmounts, requires scrub to report CLEAN and rejects any legacy FUSE executable or process.

## Current development scope

The native driver is now the product path, but 0.18.3 remains pre-1.0 development code. Native parity includes namespace mutation, random and sparse writes, truncate/fallocate, Linux xattrs and special nodes, shared writable mmap, retained-snapshot writes and mounted remount/scrub coverage. Further kernel work focuses on performance, wider stress coverage and continued locking/concurrency hardening. The portable core remains the canonical on-disk transaction and validation model shared by Linux and Windows tooling.
