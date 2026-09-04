<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Platform Adapters

This document defines the operating-system adapter boundary. It does not track release history or feature-completion evidence; use `ROADMAP.md` and `QUALIFICATION.md` for those.

## Principle

InfiltratorFS is one filesystem with multiple first-class operating-system adapters. It is not a Linux filesystem that other systems emulate.

The on-disk format and portable core define persistent meaning. Each adapter translates native APIs, caching, security, namespace and object-lifetime rules onto that common model.

The rule is:

```text
same underlying concept, different native name
    -> one common InfiltratorFS concept

similar but not identical
    -> common meaning plus preserved platform detail

genuinely platform-specific
    -> typed adapter/extension metadata

unknown on another platform
    -> retain it; do not silently destroy it
```

## Portable-core ownership

The portable core owns:

- current-format validation;
- persistent filesystem/object identity and namespace graph;
- allocation and extent semantics;
- inline, sparse, shared and compressed data representations;
- checksums and integrity rules;
- transactions, checkpoint publication and recovery;
- retained generations and snapshots;
- portable attributes; and
- scrub/forensic interpretation of the persistent format.

An adapter must not redefine these concepts merely to mirror one operating system's in-memory structures.

## Adapter ownership

An operating-system adapter owns:

- native mount/unmount registration;
- inode/vnode/file-object lifetime;
- page/cache-manager integration;
- native memory mapping;
- locking/share/open/delete semantics;
- local identity/account resolution;
- translation of ACLs and attributes;
- platform-specific special objects or extension points; and
- boot/installer integration where the filesystem is used as a system volume.

Adapter-only metadata must remain isolated from portable filesystem identity and should survive access from an operating system that cannot interpret it.

## Linux

Linux uses the native out-of-tree `infiltratorfs.ko` VFS adapter. Linux API vocabulary such as `fallocate`, FIEMAP, `FICLONE`, xattr namespaces, inode lifecycle and page-cache operations maps onto the portable object/extent/transaction model rather than becoming the portable model itself.

The former FUSE implementation is not a current adapter path.

Linux Mint's Mintstick/Nemo USB Stick Formatter is intentionally not used as an InfiltratorFS partition formatter because it is a whole-device repartitioner. Existing-partition formatting belongs to InfiltratorFS Manager or the libblockdev/UDisks/GNOME Disks path so surrounding partitions are preserved.

## Windows

Windows currently has portable-core image/raw-device access plus a user-mode Explorer bridge built on Microsoft's inbox Projected File System (ProjFS).

The bridge exposes a projected NTFS virtualization root, hydrates InfiltratorFS data on demand and persists the supported Windows mutations back through the portable core. Provider-backed directories may be materialized as ordinary local directories while file content remains lazily projected so normal Explorer move/rename behaviour can work around ProjFS partial-directory limitations.

This bridge is interoperability, not a native InfiltratorFS Windows filesystem driver. Windows still sees a projected NTFS surface, and Windows-specific kernel filesystem semantics remain outside this bridge.

A future native Windows adapter must integrate with the Windows I/O Manager, Cache Manager, Memory Manager, security descriptors, file/share/delete semantics, reparse/extension behaviour and native volume mounting while preserving the same persistent InfiltratorFS model.

Windows should not emulate Linux syscalls, and Linux should not emulate NTFS. Equivalent operations on each platform should map to the same portable semantic operation.

## macOS and BSD

A macOS or BSD adapter would map vnode operations, ownership/ACLs, xattrs, caching and namespace behaviour onto the portable model.

Resource forks or similar named data/metadata should prefer a generic named-stream/named-metadata representation where the underlying concept is portable, with typed platform metadata retained only for genuinely platform-specific semantics.

Booting a stock modern macOS installation from InfiltratorFS is a separate integration problem because Apple's boot/security chain has APFS-specific requirements. That does not change the portable filesystem architecture.

## Haiku

A Haiku adapter would map Haiku filesystem hooks and named attributes onto the same model. Haiku's indexed/queryable attributes may motivate generic indexed metadata, but portable concepts should not be introduced as opaque Haiku-only structures when they can be generalized.

System-volume support would additionally require bootloader, installer and early-boot integration.

## System-volume support

Being mountable as a data filesystem and being suitable as an operating system's root/system filesystem are different integration levels.

A system-volume adapter must satisfy normal filesystem semantics plus the platform's early-boot storage requirements. Linux can make the native driver available from initramfs before mounting `/`; other systems may have different constraints.

Those requirements belong to platform integration unless they expose a genuinely useful generic filesystem primitive.

## Metadata preservation

An adapter must not destroy metadata merely because its host operating system cannot expose it naturally.

For example, a future Windows ACL must not be irreversibly flattened to Unix mode bits merely because the volume is mounted on Linux. Likewise, Linux-specific metadata that Windows cannot represent directly should remain intact unless an explicit cross-platform policy says otherwise.

This rule is central to removable/shared volumes and to using the same filesystem across operating systems.

## Implementation test

Before adding a platform-specific persistent feature, ask:

1. What is the underlying filesystem concept?
2. Does the portable core already represent it under another name?
3. Can a generic extension represent it without losing semantics?
4. What truly remains platform-specific after that generalization?

Only the final category should become adapter-specific persistent metadata.