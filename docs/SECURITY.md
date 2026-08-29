<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Security Model

## Status

This document defines the intended cross-platform security architecture. Release 0.18.21 does **not** yet implement the final portable security-object format. Current POSIX mode/UID/GID compatibility metadata and Linux adapter metadata must therefore not be mistaken for the future canonical cross-platform identity model.

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

The portable ACL vocabulary should describe filesystem meaning rather than copy one operating system's bit constants. Candidate rights include:

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

Adapters then translate native access masks onto these rights.

Traditional POSIX read/write/execute bits are a projection of part of this model. Windows DACL rights are another projection. The portable model must be rich enough that translating through Linux does not destroy Windows-only ACL detail, and translating through Windows does not destroy POSIX-only state.

## ACL entries

A versioned portable security object should support ordered access-control entries containing at least:

- principal identity;
- allow or deny disposition;
- portable rights mask;
- inheritance/applicability flags; and
- reserved/versioned extension space for future audit or platform-specific policy.

Explicit deny and inheritance need first-class representation because reducing all permissions to POSIX mode bits would lose information that other platforms rely on.

## Linux mapping

Linux adapters may expose:

- owner/group identity;
- mode bits;
- POSIX ACLs where supported;
- Linux-specific security/xattr metadata.

`chmod`, `chown` and POSIX ACL changes require defined update rules against the portable security object. They must not blindly replace a richer ACL with only the information Linux can display.

Release 0.18.21 stores POSIX mode and numeric UID/GID compatibility metadata and supports standard Linux xattr namespaces. Those facilities are useful today but are not yet the final portable principal/ACL store.

## Windows mapping

A future Windows filesystem driver should map Windows security descriptors, SIDs, DACL entries and inheritance semantics onto the same portable security object.

Windows-specific information that has no portable equivalent should be retained in typed extension metadata rather than silently discarded when the volume is later mounted elsewhere.

Likewise, a Windows-side ACL edit must have defined projection/update semantics so it does not accidentally erase unrelated portable or platform-specific entries.

## Other operating systems

macOS, BSD, Haiku and future adapters follow the same rule: map equivalent concepts to the portable security model; preserve additional semantics separately where necessary; never make one platform's identity namespace the filesystem's permanent authority.

The same approach applies to adjacent metadata classes such as named attributes, resource forks, alternate data streams and typed/reparse metadata: first identify the underlying portable meaning, then preserve genuinely platform-specific additions without data loss.

## On-disk direction

Format 0.17 already reserves a `security object ID` in common attributes. That reference remains zero in the current portable format because the security-object record class and feature/version contract have not yet been standardized.

A future format revision should define:

- versioned security-object records;
- stable principal IDs and typed platform bindings;
- versioned ACL entries and rights;
- inheritance semantics;
- unknown-entry preservation rules;
- checksumming and graph reachability requirements; and
- scrub/recovery behavior for security metadata.

Because InfiltratorFS is still pre-1.0, this can be introduced cleanly without preserving compatibility with older development formats.

## Security invariants

Once the portable security format exists, conformance should require that:

- access decisions never depend on unauthenticated security metadata;
- unknown security extensions are preserved or rejected according to explicit feature/version rules;
- an adapter never widens access merely because it cannot represent a restriction in its normal UI;
- unresolved principals remain stable and non-destructive;
- ACL/security updates participate in the same transactional publication and checksum model as other critical metadata; and
- scrub validates security-object identity, ownership/reference relationships, checksums and canonical encoding.
