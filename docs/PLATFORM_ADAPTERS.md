<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Platform Adapters

## Purpose

InfiltratorFS is intended to be one filesystem with multiple first-class operating-system adapters, not a Linux filesystem that other systems emulate.

The on-disk format and portable core define persistent meaning. Each adapter translates native APIs, caching, security, namespace and object-lifetime rules onto that common model.

## Common versus platform-specific semantics

The adapter rule is:

```text
same underlying concept, different native name
    -> one common InfiltratorFS concept

similar but not identical
    -> common meaning plus preserved platform detail

genuinely platform-specific
    -> preserve as typed adapter/extension metadata

unknown on another platform
    -> retain it; do not silently destroy it
```

Examples of concepts that are fundamentally portable include regular files, directories, persistent object identity, links, logical size, allocated extents, sparse ranges, timestamps, transactions, snapshots, checksums and access-control intent.

Examples of adapter work include translating Linux VFS operations, Windows I/O and cache-manager rules, macOS vnode behavior, BSD vnode/flag behavior or Haiku filesystem hooks into those portable concepts.

## Adapter boundary

The portable core owns:

- current-format validation;
- persistent object IDs and namespace graph;
- allocation and extent semantics;
- inline, sparse and shared data representations;
- checksums and integrity rules;
- transactions, checkpoint publication and recovery;
- retained generations and snapshots;
- portable attributes; and
- scrub/forensic format interpretation.

An operating-system adapter owns:

- native mount/unmount registration;
- native inode/vnode/file-object lifetime;
- page/cache-manager integration;
- native `mmap`/memory-mapping behavior;
- locking/share/open/delete semantics;
- local identity/account resolution;
- translation of ACLs and attributes;
- platform-specific special objects or extension points; and
- boot/installer integration when InfiltratorFS is used as a system volume.

The adapter must not redefine the persistent format merely to mirror one platform's in-memory structures.

## Linux

Linux is currently the most complete mounted adapter. Current Linux source uses `infiltratorfs.ko` and exposes current Format 0.17 through Linux VFS with native namespace mutation, random/sparse writes, truncate, `fallocate`, hole punching, crash-safe open-unlink lifetime, standard xattr namespaces, special nodes, page-cache/readahead/shared-`mmap` integration, snapshot-preserving live writes, checkpoint fallback/replica healing, online defragmentation, 64-shard volatile allocation reservations, bounded multi-process locking/concurrency qualification, fail-closed metadata-tree alias/cycle detection, and kernel-stack-safe allocation-tree traversal.

These Linux implementation details are not requirements that another adapter copy line-for-line. For example, Linux `fallocate` flags are Linux API vocabulary; the underlying portable concepts are preallocation, sparse ranges and hole punching.

## Windows

The repository contains Win32 image/raw-partition storage and direct transfer/scrub tooling over the portable core. Release 0.18.30 introduced an experimental driverless Explorer bridge built on Microsoft's inbox Projected File System (ProjFS). After the Windows application opens an InfiltratorFS partition, **Mount in Explorer** starts a user-mode provider and exposes a projected NTFS virtualization root. Explorer opens that root directly, rather than depending on a DOS drive alias created by the elevated raw-device process; an auxiliary drive alias is retained only when Windows exposes it in the current UAC device namespace. File data is hydrated from the portable InfiltratorFS core on demand, and Windows-created/modified/deleted/renamed files are committed back to the same InfiltratorFS volume. InfiltratorFS ships no Windows kernel driver for this bridge; the kernel component is Microsoft's signed ProjFS filter already supplied by Windows as an optional feature.

The bridge is intended for ordinary cross-platform file work while the native Windows filesystem driver remains future work. It is not equivalent to that future driver: Windows still sees a projected NTFS virtualization root, Windows-incompatible names or metadata may not have a native Explorer representation, and boot/system-volume, Cache Manager, security-descriptor, reparse-point and full native share/delete semantics still belong to the eventual filesystem driver.

A true Windows filesystem driver will need Windows-native integration for I/O Manager, Cache Manager, Memory Manager, security descriptors, file-object/share/delete semantics, reparse/extension behavior and native drive mounting.

Where Windows and Linux express the same underlying operation differently, both adapters should call or reproduce the same portable InfiltratorFS semantic operation. Windows should not emulate Linux syscalls, and Linux should not emulate NTFS.

## macOS and BSD

A macOS or BSD adapter would map vnode operations, ownership/ACLs, xattrs, caching and namespace behavior to the same core. Resource forks or other named data/metadata should be represented through a generic named-stream/named-metadata facility where possible, with typed platform metadata retained when necessary.

Booting a stock modern macOS installation from InfiltratorFS is a separate platform-integration problem because Apple's current boot and security chain is tied to APFS-specific volume-group and signed-system-volume machinery. That constraint does not change the filesystem's platform-neutral architecture.

## Haiku

A Haiku adapter would map Haiku vnode/filesystem hooks and its heavy use of named attributes onto the common filesystem model. Haiku's indexed/queryable attributes may motivate generic indexed metadata in the portable core, but they should not be introduced as opaque Haiku-only structures if the underlying concept can be generalized.

Using InfiltratorFS as a Haiku system volume would additionally require bootloader, installer and early-boot support, just as Linux system-volume use requires the driver to be available before the root filesystem is mounted.

## System-volume and boot support

Being mountable as a data filesystem and being suitable as an operating system's root/system filesystem are different integration levels.

A system-volume adapter must satisfy all ordinary filesystem semantics plus whatever the platform requires before its normal filesystem driver stack is fully available. Linux can load an InfiltratorFS driver from an initramfs and then mount InfiltratorFS as `/`. Other operating systems may have more tightly coupled boot/storage requirements.

Those boot requirements belong to platform integration unless they expose a genuinely useful generic filesystem primitive.

## Metadata preservation

An adapter must never destroy metadata merely because its host operating system cannot expose it naturally.

For example, a sophisticated ACL created on Windows must not be flattened irreversibly to Unix mode bits simply because the volume was mounted on Linux. Likewise, Linux-specific metadata that Windows cannot represent directly should remain intact unless an explicit cross-platform policy says otherwise.

This preservation rule is central to removable/shared volumes and to using the same InfiltratorFS media across operating systems.

## Implementation guidance

Before adding a platform-specific on-disk feature, ask:

1. What is the underlying filesystem concept?
2. Does the portable core already represent it under another name?
3. Can a generic extension represent it without losing semantics?
4. What truly remains platform-specific after that generalization?

Only the final category should become adapter-specific persistent metadata.
