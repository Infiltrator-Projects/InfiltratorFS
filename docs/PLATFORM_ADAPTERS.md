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

The bridge exposes a projected NTFS virtualization root, hydrates InfiltratorFS data on demand and persists Windows mutations back through the portable core. Provider-backed directories may be materialized as ordinary local directories while file content remains lazily projected so normal Explorer move/rename behaviour can work around ProjFS partial-directory limitations.

The completed metadata adapter round-trips Windows basic attributes and all four portable timestamps, maps owner/group/DACL state through the portable principal/security-object model, preserves ACE ordering and inheritance flags for ordinary allow/deny file-system ACEs, and projects every valid InfiltratorFS UTF-8 component without destructive renaming. Components that Win32 cannot represent directly—including DOS-reserved names, names with Win32-forbidden characters, trailing dot/space, the bridge-reserved prefix, and names longer than the NTFS component limit—receive deterministic SHA-256 projection aliases whose authoritative InfiltratorFS path remains attached to the provider identity. New ordinary Windows names continue to map directly to UTF-8.

The Windows Manager shares its application/presentation contract with the Linux Manager: the storage-target model, Overview structure, maintenance action set, wording and Common appearance roles are one product contract. Win32 owns only native control rendering, storage discovery/dialogs and Windows-specific file/Explorer integration. The Files page is an adapter capability because the current Windows path must provide userspace import/projection while Linux can hand an already-mounted filesystem to the native desktop file manager.

This bridge is interoperability, not the native InfiltratorFS Windows filesystem driver. Windows still sees a projected NTFS surface when this path is selected.

The repository now also contains a native Windows disk-filesystem driver plus companion portable-core service. The kernel side owns I/O Manager/Cache Manager integration, native volume mounting, FCB/CCB and section-object lifetime, cached/non-cached/paging I/O, share/delete semantics, byte-range locks, oplocks, security dispatch and durability boundaries; the service reuses the authoritative portable filesystem core rather than introducing a second Windows-only on-disk implementation. The source implementation is complete enough to build and qualify structurally, but release qualification still requires a real mounted Windows volume and Driver Verifier run.

Windows should not emulate Linux syscalls, and Linux should not emulate NTFS. Equivalent operations on each platform should map to the same portable semantic operation.

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