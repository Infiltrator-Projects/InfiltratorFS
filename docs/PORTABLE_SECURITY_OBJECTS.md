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
- fixed-size canonical binding records whose meaningful byte length is explicit.

A principal may therefore carry both POSIX and Windows bindings at the same
time. A platform binding is never itself the portable identity.

POSIX numeric identities are scoped. A POSIX UID/GID binding contains a
nonzero 128-bit identity-authority ID followed by the little-endian 32-bit
numeric UID/GID. The authority identifies the account namespace in which that
number has meaning; bare UID/GID numbers are never globally resolvable.

A Windows binding stores the canonical binary SID representation. Revision,
sub-authority count and exact encoded length are validated. Textual SID strings
are an adapter/UI representation only.

Opaque bindings are preservation-only. They may be duplicated and round-tripped,
but they are never used for credential resolution or reverse principal lookup.

For resolvable bindings, writers reject a second principal with the same
canonical binding and scrub detects duplicates.

## Well-known principals

A small reserved principal-ID namespace is implicit and is never stored as
ordinary principal objects. The first required semantics are:

- OWNER — resolves to the descriptor's owner principal;
- GROUP — resolves to the descriptor's primary-group principal;
- EVERYONE — applies to every subject;
- CREATOR_OWNER — inheritance template replaced by the child owner; and
- CREATOR_GROUP — inheritance template replaced by the child primary group.

CREATOR_OWNER and CREATOR_GROUP are valid only on inheritable, inherit-only ACE
templates and never participate directly in an access check. Ordinary generated
object IDs must never enter the reserved well-known namespace.

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

The portable ACL evaluator is ordered and deny-aware. OWNER, GROUP and
EVERYONE are resolved by portable semantics before ordinary principal matching;
creator principals are inheritance templates and are never access subjects.

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

Descriptor and principal reachability is authoritative from the committed
namespace/security graph, not from a persistent mutable reference count.
Foreground ACL detach/unlink must not scan the whole namespace merely to prove
that an object became unreachable. Reclamation is a tracing/GC responsibility
of scrub or a bounded maintenance pass, with retained snapshots protecting
their historical blocks through the ordinary generation/allocation model.

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

The native Linux driver continues to use its established POSIX compatibility
metadata and VFS ACL enforcement as the Linux projection. Portable-security
completion requires the persistent objects, ordered evaluator, inheritance,
Linux/Windows projection rules, native-reader acceptance and exact-source
qualification to agree on this contract. Platform-specific UI/driver features
that go beyond this portable contract remain separate adapter roadmap items.

## Development format boundary

The current portable-security payload version is 2. It replaces all prior
development-only security-object representations. The incompatible feature bit
selects the portable-security object family; the object payload version selects
its exact current representation.

InfiltratorFS has not reached a stable format. Superseded development security
payloads are rejected; no backward reader, migration path or compatibility
shim is required.
