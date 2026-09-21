<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Portable Security Objects

This document is the design contract for InfiltratorFS portable principals and
ACL security descriptors. The byte-level authority remains
`include/infilfs/format.h`; `SECURITY.md` describes security policy and threat
boundaries.

## Goals

The persistent security model must remain independent of Linux UID/GID numbers,
POSIX ACL encodings, Windows SID/ACCESS_MASK encodings and future platform ABI
choices. Moving a volume between operating systems must preserve identities,
ACL intent and unknown bindings without flattening them into the currently
mounted operating system's representation.

The model therefore separates three concepts:

1. a stable InfiltratorFS principal identity;
2. one or more platform bindings for that principal; and
3. an immutable shareable security descriptor containing owner/group identities
   and ordered ACL entries.

## Principal catalog

The volume-level principal catalog is represented by ordinary indexed
`INFS_OBJECT_PRINCIPAL` objects rather than one monolithic catalog block. The
object index is already the scalable persistent ID-to-object catalog, so using
one object per principal avoids introducing a second fixed-capacity directory.

A principal object's 128-bit object ID is also its stable principal ID. A
principal stores:

- principal kind: user, group, service or well-known;
- zero or more typed platform bindings;
- versioned flags; and
- canonical variable-length binding records.

A principal may therefore carry both a POSIX UID/GID binding and a Windows SID
binding at the same time. A platform binding is never itself the portable
identity.

For bindings whose platform namespace requires uniqueness, writers reject a
second principal with the same binding and scrub detects duplicates. Unknown
binding types are either preserved under an explicitly opaque type or rejected
according to the selected record version.

## Shareable security descriptors

`infs_attributes_disk.security_object_id` references an indexed
`INFS_OBJECT_SECURITY` descriptor. Security descriptors are immutable in
meaning and may be referenced by any number of files/directories.

A descriptor stores:

- owner principal ID;
- primary-group principal ID;
- versioned descriptor/control flags;
- ordered ACEs; and
- a canonical semantic digest used for exact descriptor reuse.

The descriptor object has no namespace parent. Its header parent ID is zero
because ownership is many-to-one from namespace objects to a shared descriptor.

Setting an ACL first canonicalizes the requested descriptor and then reuses an
existing descriptor when the semantic digest and full canonical payload match.
Otherwise a new descriptor is created. Detaching a descriptor never destroys it
until the current committed namespace graph has no remaining references.
Reference ownership is therefore derived from the graph rather than trusted to
a mutable on-disk reference counter.

This keeps descriptors shareable and avoids creating a second consistency
problem around persistent reference counts. A later runtime cache may accelerate
descriptor lookup/reclamation without changing the format.

## ACL semantics

The portable ACL evaluator is ordered and deny-aware, following the proven
NFSv4/Windows style rather than reducing access to POSIX rwx bits.

For a requested rights mask:

1. ACEs are considered in stored order.
2. ACE applicability/inheritance flags are evaluated for the target object.
3. An applicable DENY ACE rejects any still-undecided requested right it covers.
4. An applicable ALLOW ACE grants still-undecided requested rights it covers.
5. Evaluation may stop when every requested right is decided.
6. Any requested right still undecided at the end is denied.

The persistent right numbers remain InfiltratorFS-defined. Linux mode/POSIX ACL
and Windows ACCESS_MASK values are adapter projections only.

Descriptor flags explicitly distinguish ACL presence from an empty ACL and
reserve protection/automatic-inheritance state so adapters do not have to infer
semantics from ACE count.

## Inheritance

ACE inheritance is represented explicitly with file-inherit,
directory-inherit, inherit-only, no-propagate and inherited flags. Child
creation derives a new canonical descriptor from the parent descriptor plus
adapter-requested owner/group changes; it does not mutate the parent's
descriptor in place.

## Validation and scrub

Readers and scrub fail closed on:

- malformed principal or descriptor payload sizes;
- duplicate principal IDs;
- duplicate unique platform bindings;
- ACE references to missing principals;
- invalid/unknown rights, dispositions or flags for the selected version;
- nonzero reserved bytes;
- namespace security references to the wrong object type;
- descriptors whose owner/group principals do not exist;
- digest/payload disagreement; or
- illegal descriptor graph relationships.

Scrub derives actual descriptor reference counts from the current namespace
graph. Unreferenced descriptors may be reclaimed deterministically; they are
not considered reachable security state merely because they remain indexed.

## Scaling

Principal count scales through the ordinary object index. Descriptor count also
scales through the object index. The first descriptor representation may keep a
compact inline ACE array when it fits one object; descriptors that exceed the
inline capacity use ordinary checksummed metadata pages under the same object
identity. No public ACL limit is defined by the size of one 4096-byte object.

## Adapter boundary

Linux and Windows adapters resolve local credentials into principal IDs through
typed bindings, project native permissions onto the portable rights mask and
preserve platform-specific security metadata that has no portable equivalent.

The native Linux driver may continue using its established POSIX compatibility
metadata while the portable evaluator is being wired into enforcement, but
portable security objects are not considered roadmap-complete until persistent
objects, inheritance, access evaluation, platform mapping and qualification all
agree on this contract.

## Pre-1.0 compatibility

This redesign replaces the earlier development-only embedded-principal security
object representation. InfiltratorFS has not reached a stable format, so no
reader or migration path for the superseded development representation is
required.
