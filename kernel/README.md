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

`infiltratorfs.ko` is a composite Kbuild module rather than a single giant translation unit. `infiltratorfs_core.c` owns VFS, mount and checkpoint orchestration, `infiltratorfs_allocation_map.c` owns Format 0.17 allocation-map loading, `infiltratorfs_resize.c` owns mounted geometry transitions, `infiltratorfs_index_tree.c` owns Format 0.17 object-index reads and snapshots, `infiltratorfs_parallel_alloc.c` owns volatile sharded data reservations, `infiltratorfs_allocation_publish.c` owns CoW allocation-tree publication, and `infiltratorfs_read_cache.c` owns verified read cursor/page caching. Shared native-driver state and the deliberately narrow cross-object API live in the private `infiltratorfs_internal.h` header.

Some already-qualified native layers are still textually composed inside the core object, including write-data paths, namespace mutation, directory trees, page cache, Linux metadata, quotas and defragmentation. Those include units are localized migration debt, not the module architecture and not a pattern for new features. Additional layers should move behind explicit internal APIs as independently compiled objects when their dependency boundaries are proven.

The important design rules are:

- the portable format remains canonical;
- persistent allocation is authoritative, while reservation/placement state is volatile;
- mutation follows the transaction/checkpoint durability model;
- Linux sidecar metadata does not become portable identity/security semantics;
- snapshot/reflink ownership must be respected by write, defrag and reclamation paths; and
- malformed metadata topology must fail closed without unbounded recursion or kernel-stack use.

`kernel/Makefile` contains the canonical component and synchronization contract. `infiltratorfs_ioctl.h` carries synchronization rules with the DKMS source itself, and `tests/native-kernel-maintainability-policy.sh` requires the composite Kbuild boundary, prevents extracted allocation/index/resize/reservation/publication/read-cache components from regressing into textual inclusion, blocks new nested `.inc` dependencies and macro-alias layering, and constrains growth of the remaining migration units.

New native functionality should use explicit helper names and existing ownership boundaries. Behaviour-preserving component extraction must retain the same policy and mounted qualification coverage; structural cleanup is not a reason to mix unrelated filesystem changes into the same refactor.

## Locking contract

The source-level deadlock-prevention contract is intentionally not duplicated here. See the synchronization contract in `infiltratorfs_ioctl.h` and the fuller implementation/composition contract in `Makefile`. The maintainability policy guard verifies those contracts and the current acquisition/include relationships in CI.

## Qualification

The `Native Linux kernel module` workflow is the ordinary mounted kernel qualification path for relevant kernel/core/package changes. Dedicated workflows may qualify independent capabilities such as resize, while million-file/1 TiB and endurance workloads live in the separate weekly/manual Heavy qualification workflow.

Do not copy workflow results or current feature status into this README. `../docs/QUALIFICATION.md` is the evidence ledger and `../docs/ROADMAP.md` is the feature-status source.

The release publisher additionally installs the generated package and performs an installed native mount/write/read/unmount/scrub gate before publication.

For a comprehensive explicitly destructive physical-media audit, use the repository's maintained `tests/native-complete-qualification.sh` harness on dedicated test media.