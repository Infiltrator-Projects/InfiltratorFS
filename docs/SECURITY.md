<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Security Model

## Status

This document defines the cross-platform security architecture. Current source implements versioned portable security objects with stable 128-bit principal IDs, typed platform bindings, ordered allow/deny ACL entries and the portable rights vocabulary. Linux/POSIX and Windows security-descriptor projection policies are separate adapter contracts. Current POSIX mode/UID/GID compatibility metadata and Linux adapter xattrs remain compatibility/sidecar state rather than the canonical portable principal/ACL store.

## Threat model and trust boundaries

InfiltratorFS treats persistent media as untrusted input. A malformed, partially
written or deliberately crafted image/device must not be able to bypass format
bounds, graph ownership rules, checksum validation or canonical-encoding rules.
Readers, scrubbers and adapters are expected to reject structures whose
integrity or interpretation cannot be established.

The portable core trusts the storage callback implementation to perform the
requested I/O against the intended target and to honour successful durability
operations. It does not treat a successful device read as proof that the bytes
are valid filesystem metadata. Operating-system adapters additionally trust
their host kernel/runtime for process isolation, credential enforcement and the
correct implementation of native privilege boundaries.

Administrative raw-device operations are privileged by design. The filesystem
cannot protect a mounted or offline volume from an actor who already has
sufficient host privilege to overwrite the underlying block device or replace
the running kernel/module.

The current integrity model primarily addresses accidental corruption,
torn/incomplete publication and malformed persistent state. CRC64 metadata
checksums and SHA-256 logical data digests are integrity mechanisms, not keyed
authentication. They do not establish that bytes came from a trusted writer.

## Current guarantees and non-goals

Current Format 0.18 provides structural validation, checksummed metadata,
logical-data digest verification, copy-on-write publication, checkpoint
recovery and fail-closed handling of unsupported/malformed persistent
structures.

Current Format 0.18 does **not** provide confidentiality for offline media.
Whole-volume/per-file encryption, key wrapping and authenticated encrypted
metadata remain roadmap work. An attacker who can read the raw medium can read
unencrypted file data and metadata; an attacker who can arbitrarily rewrite the
medium is outside the guarantees of unkeyed checksums.

Availability under hostile resource-exhaustion input is bounded where practical
through explicit record limits, traversal guards and malformed-topology
rejection, but the project does not claim resistance to every denial-of-service
strategy available to a privileged local attacker or a failing storage device.

## Design rule

InfiltratorFS security belongs to InfiltratorFS, not to Linux, Windows or another operating system.

When two operating systems expose the same underlying security concept under different names, adapters map both to one portable InfiltratorFS concept. When an operating system has additional semantics that another platform cannot express, that information is preserved rather than flattened or destroyed.

Mounting a volume on an operating system that cannot expose a particular security feature must not silently erase that feature.

## Portable principals

A future security object should refer to stable InfiltratorFS principals rather than directly treating a Linux numeric UID or a Windows SID as the filesystem-wide identity.

Conceptually a principal contains a stable persistent identifier plus zero or more platform identity bindings, for example:

```text
InfiltratorFS principal
    persistent principal ID
    Linux UID/GID binding(s)
    Windows SID binding(s)
    future platform identity binding(s)
```

The persistent principal remains the same object even when a volume moves between operating systems. Adapter bindings determine how the local OS resolves that principal.

Unresolved principals must remain intact. A platform must be able to preserve an ACL entry even when it cannot map the referenced principal to a current local account.

## Portable access rights

`include/infilfs/security.h` defines the stable portable rights mask. Its bit
positions describe filesystem meaning and never copy Linux mode/POSIX ACL or
Windows ACCESS_MASK constants:

```text
read data
write data
append data
execute
list directory
traverse directory
create file
create directory
delete
delete child
read attributes
write attributes
read named metadata
write named metadata
read permissions
change permissions
take ownership
```

Adapters translate native access rules onto these rights. Traditional POSIX
read/write/execute bits are one projection; Windows DACL rights are another.
The vocabulary is deliberately available before the final security-object
record exists so persistent ACL design cannot accidentally make one platform's
numeric ABI authoritative.

## ACL entries

A versioned portable security object should support ordered access-control entries containing at least:

- principal identity;
- allow or deny disposition;
- portable rights mask;
- inheritance/applicability flags; and
- reserved/versioned extension space for future audit or platform-specific policy.

Explicit deny and inheritance need first-class representation because reducing all permissions to POSIX mode bits would lose information that other platforms rely on.

## Linux mapping

Linux exposes owner/group identity, mode bits, POSIX ACLs and Linux-specific
security/xattr metadata. The mapping policy is explicit:

- numeric UID and GID values are adapter-local compatibility bindings, never
  portable principal IDs;
- for regular files, POSIX read maps to `read data`, write maps to both
  `write data` and `append data`, and execute maps to `execute`;
- for directories, POSIX read maps to `list directory`, write maps to
  `create file`, `create directory` and `delete child`, and execute maps
  to `traverse directory`; an operation may require both write and traverse;
- a POSIX ACL entry uses the same rwx projection after the ACL mask/effective
  permissions are applied by the Linux VFS;
- mode/ACL bits do not manufacture attribute, permission-administration or
  ownership rights that POSIX controls through ownership/capability rules;
- `chmod`, `chown` and POSIX ACL updates follow Linux VFS semantics and
  update the Linux compatibility/sidecar state without treating that state as
  the final portable ACL object.

The executable projection helpers are in `include/infilfs/posix_security.h`.
Mounted qualification exercises real `setfacl`/`getfacl`, chmod mask
interaction, default-ACL inheritance, rsync ACL preservation, remount
durability and enforcement through an unprivileged UID. Current Linux metadata
remains a compatibility layer until versioned portable security objects and
principals are implemented.

## Windows mapping

Windows SIDs are adapter identity bindings, not portable principal IDs. A DACL
ACE keeps its allow/deny disposition and ordering, resolves the SID through the
principal binding table when possible, and projects its ACCESS_MASK onto the
portable rights vocabulary. Unresolved SIDs remain intact as bindings so moving
the volume between machines or domains is non-destructive.

The executable ACCESS_MASK projection is in
`include/infilfs/win32_security.h`. File and directory meanings are kept
distinct where Windows overloads the same specific-access bits:

- file read/write/append/execute map to the corresponding portable data rights;
- directory list/add-file/add-subdirectory/traverse/delete-child map to the
  corresponding portable directory rights;
- read/write extended attributes map to read/write named metadata;
- read/write attributes map to portable attribute rights;
- DELETE, READ_CONTROL, WRITE_DAC and WRITE_OWNER map to delete, read
  permissions, change permissions and take ownership respectively;
- GENERIC_READ/WRITE/EXECUTE/ALL are expanded with the Windows file-object
  generic mapping before the specific bits are interpreted; and
- SYNCHRONIZE, ACCESS_SYSTEM_SECURITY and MAXIMUM_ALLOWED do not manufacture
  portable ACL rights because they are host object-manager/request semantics,
  not persistent filesystem permissions.

DACL inheritance flags and explicit allow/deny entries are preserved by the
portable security object rather than flattened into a Unix-style rwx mask.
SACL/audit policy and Windows-specific control bits that have no portable
equivalent are preserved as typed platform-specific security metadata rather
than silently discarded. A Windows-side ACL edit must update the represented
portable entries without erasing unrelated unknown/platform-specific metadata.

## Other operating systems

macOS, BSD, Haiku and future adapters follow the same rule: map equivalent concepts to the portable security model; preserve additional semantics separately where necessary; never make one platform's identity namespace the filesystem's permanent authority.

The same approach applies to adjacent metadata classes such as named attributes, resource forks, alternate data streams and typed/reparse metadata: first identify the underlying portable meaning, then preserve genuinely platform-specific additions without data loss.

## On-disk security objects

Format 0.18 now uses the reserved `security object ID` in common attributes
when incompatible feature `SECURITY_OBJECTS_V1` is present. A nonzero
reference points to a version-1 portable security object owned by that namespace
object.

The record contains stable principal IDs with typed platform bindings followed
by ordered ACL entries. ACL entries carry allow/deny disposition, portable
rights and inheritance/applicability flags. The record is checksummed like
other objects, participates in copy-on-write publication, is indexed by its
persistent object ID and is validated as part of the ownership/security graph.

Unknown rights or flags fail closed in v1. Duplicate principals, ACEs that
reference absent principals, malformed binding lengths, dangling owner
references and namespace/security back-reference disagreement are corruption.
Platform-specific security information that cannot yet be represented by v1
remains a separate preservation-layer roadmap item rather than being silently
flattened into the portable ACL.

## Security invariants

Once the portable security format exists, conformance should require that:

- access decisions never depend on unauthenticated security metadata;
- unknown security extensions are preserved or rejected according to explicit feature/version rules;
- an adapter never widens access merely because it cannot represent a restriction in its normal UI;
- unresolved principals remain stable and non-destructive;
- ACL/security updates participate in the same transactional publication and checksum model as other critical metadata; and
- scrub validates security-object identity, ownership/reference relationships, checksums and canonical encoding.
