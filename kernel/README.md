<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Native Linux kernel module

This directory begins the native Linux VFS adapter for InfiltratorFS. It is an out-of-tree kernel module: Linux itself does not need to be rebuilt. The module is built against the installed kernel headers and will ultimately be packaged through DKMS so normal kernel upgrades rebuild only `infiltratorfs.ko`.

The first milestone is deliberately narrow and read-only. The module registers the `infiltratorfs` filesystem type, requires a block-device mount, reads the physical block-zero checkpoint, checks the InfiltratorFS magic, current Format 0.12 identity, 4096-byte block size and nonzero generation, and exposes a read-only VFS root inode. It does not yet enumerate the real root directory or read files; those are the next adapter stages.

Build against the running kernel when matching headers are installed:

```bash
make -C kernel
```

Or build against an explicit kernel-header tree:

```bash
make -C kernel KDIR=/usr/src/linux-headers-$(uname -r)
```

The resulting module is `kernel/infiltratorfs.ko`. Loading it is a normal module operation (`insmod`/`modprobe`), not a kernel rebuild. Until namespace and file reads are implemented, this bootstrap should only be used with disposable test media and explicit read-only mounts.

The implementation intentionally does not include the userspace portable-core headers directly. Kernel code cannot rely on libc types or allocation/runtime services. A later stage will factor the persistent packed layout into a kernel-safe shared format header and add kernel implementations for storage I/O, allocation, time, randomness and synchronization while keeping the existing platform-neutral filesystem rules authoritative.
