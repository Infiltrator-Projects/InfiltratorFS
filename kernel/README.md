<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Native Linux kernel module

This directory contains the native Linux VFS adapter for InfiltratorFS. It is an out-of-tree kernel module built against installed kernel headers and packaged through DKMS; Linux itself does not need to be rebuilt.

Release 0.18.7 is the current normal Linux filesystem path. The old userspace FUSE adapter has been removed from the source tree and survives only in Git history.

The module:

- registers the `infiltratorfs` filesystem type with Linux VFS;
- mounts current Format 0.12 block devices read-only or read-write;
- selects the highest-generation structurally valid committed checkpoint graph from the three physical checkpoint locations;
- requires current Format 0.12 and rejects unknown incompatible feature bits;
- resolves classic and paged object indexes/directories;
- creates stable Linux inode identities from persistent InfiltratorFS object IDs;
- exposes regular files, directories, symbolic links, FIFOs, sockets and character/block special-node identity through native VFS objects;
- reads inline data, ordinary extents, sparse holes and paged extents directly from the block device;
- performs create/mkdir/mknod, link/symlink, rename/unlink/rmdir and persistent setattr through the native transaction path;
- supports sequential/random extent writes, large sparse growth, high-offset writes, truncate, `fallocate` and hole punching;
- preserves open-unlinked files until final inode eviction and performs mount-time orphan recovery;
- persists standard Linux xattr namespaces and adapter-specific special-node metadata;
- integrates page-cache faults, readahead and shared writable `mmap` writeback;
- preserves retained snapshot generations while live namespace/data updates continue;
- reports logical and allocated blocks for native objects;
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

The dedicated `Native Linux kernel module` GitHub Actions workflow compiles the module against Ubuntu kernel headers, reproduces the DKMS source-root build, checks module metadata and, when matching running-kernel headers are available, loads the module and performs real loop-device native read/write qualification. The mounted suite covers namespace operations, random/sparse writes, high-offset sparse files, truncate, allocation reporting, `fallocate`, hole punching, xattrs, special nodes, page-cache/readahead/mmap behavior, open-unlink lifetime, snapshot-preserving writes, remount readback and offline scrub.

The release publisher adds an installed-package gate: it installs the generated `.deb`, verifies `/proc/filesystems` and `modinfo`, mounts a real Format 0.12 image as `infiltratorfs`, writes and byte-compares non-zero data, syncs, unmounts, requires scrub to report CLEAN and rejects any legacy FUSE executable or process.

## Current development scope

The native driver has passed the major migration milestone: the current product path no longer depends on restoration of FUSE-era functionality. Follow-on work is focused on deeper checkpoint-recovery qualification, broader xattr and allocation-reporting semantics, explicit mounted regression coverage, locking/concurrency, scale, fragmentation, near-full behavior and long-running mixed-workload stress.

The portable core remains the canonical on-disk transaction and validation model shared by every operating-system adapter. Linux VFS code is therefore not intended to become the definition that a future Windows, macOS, BSD or Haiku implementation must copy.
