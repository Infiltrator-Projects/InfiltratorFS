<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Native Linux kernel module

This directory contains the native Linux VFS adapter for InfiltratorFS. It is an out-of-tree kernel module built against installed kernel headers and packaged through DKMS; Linux itself does not need to be rebuilt.

This README is intentionally limited to local kernel-module guidance. Filesystem architecture belongs in `../docs/ARCHITECTURE.md`, feature completion in `../docs/ROADMAP.md`, and qualification evidence in `../docs/QUALIFICATION.md`.

## Role

The module registers filesystem type `infiltratorfs` and maps Linux VFS operations onto the portable Format 0.17 object, extent, transaction, checkpoint and integrity model.

The current product path is native VFS only. The former userspace FUSE implementation is not part of the current source or package path and survives only in Git history.

Linux-specific metadata such as ownership/mode, xattr namespaces and special-node details is kept in adapter metadata rather than redefining the portable filesystem model.

## Build

Build against the running kernel when matching headers are installed:

```bash
make -C kernel KDIR=/lib/modules/$(uname -r)/build
```

The result is `kernel/infiltratorfs.ko`.

Load the installed module normally:

```bash
sudo modprobe infiltratorfs
```

Mount a current Format 0.17 volume:

```bash
sudo mount -i -t infiltratorfs -o rw /dev/<partition> /mnt/infiltratorfs
findmnt -T /mnt/infiltratorfs -o SOURCE,FSTYPE,OPTIONS
```

`FSTYPE` must report `infiltratorfs`.

The Debian package and native `.run` installer install a self-contained DKMS source tree under `/usr/src/infiltratorfs-<version>`. The packaging/workflow manifests are authoritative for the exact source files copied into that tree; this README deliberately does not duplicate that list.

Matching running-kernel headers are required for a native build. Installation must fail rather than silently substitute another filesystem implementation.

## Source organization

The kernel implementation is assembled from the main VFS source plus focused include units for major concerns such as allocation publication, parallel reservation, read/write data paths, namespace mutation, indexes, directory trees, page cache, Linux metadata, resize, quotas and defragmentation.

The important design rule is separation of responsibility rather than any particular filename list:

- the portable format remains canonical;
- persistent allocation is authoritative, while reservation/placement state is volatile;
- mutation follows the transaction/checkpoint durability model;
- Linux sidecar metadata does not become portable identity/security semantics;
- snapshot/reflink ownership must be respected by write, defrag and reclamation paths; and
- malformed metadata topology must fail closed without unbounded recursion or kernel-stack use.

## Locking contract

The native driver deliberately does not have one giant filesystem mutex. When a path needs more than one synchronization domain, preserve these ordering constraints rather than inferring a new order from a convenient call site:

- `resize_lock` owns geometry-change coordination and `resize_active`. Resize sets that gate before draining writers and may then acquire `write_lock`; code holding `write_lock` must therefore not acquire `resize_lock`.
- `quota_lock` owns quota rules, project-root topology and live quota accounting. Project/accounting transitions may acquire `write_lock` while `quota_lock` is held; ordinary write paths must not acquire `quota_lock` while holding `write_lock`. In particular, quota reservation finish/abort happens after the writer releases `write_lock`.
- `write_lock` serializes mutation of persistent transaction/topology state. Long-running work that does not need that state should stay outside it.
- `bitmap_lock` protects short-lived publication/visibility of allocation bitmap state. It is a non-sleeping inner lock: do not acquire mutexes or perform sleeping work while holding it.
- allocation-reservation shard spinlocks protect only volatile reservation ranges. They are shorter-lived still; reservation code may enter bitmap protection after taking the relevant shard locks, but must not sleep or acquire filesystem mutexes while a shard spinlock is held.
- `linux_meta_lock` serializes compound Linux sidecar operations. If a future path needs to nest it with another filesystem mutex, make that relationship explicit at the call site and update this contract rather than creating an undocumented reverse order.

These are deadlock-prevention constraints, not a claim that every operation takes every lock. Comments beside exceptional nested acquisitions should explain why the nesting is safe; ordinary single-lock operations do not need narration.

## Qualification

The `Native Linux kernel module` workflow is the ordinary mounted kernel qualification path for relevant kernel/core/package changes. Dedicated workflows may qualify independent capabilities such as resize, while million-file/1 TiB and endurance workloads live in the separate weekly/manual Heavy qualification workflow.

Do not copy workflow results or current feature status into this README. `../docs/QUALIFICATION.md` is the evidence ledger and `../docs/ROADMAP.md` is the feature-status source.

The release publisher additionally installs the generated package and performs an installed native mount/write/read/unmount/scrub gate before publication.

For a comprehensive explicitly destructive physical-media audit, use the repository's maintained `tests/native-complete-qualification.sh` harness on dedicated test media.