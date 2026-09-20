<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Future Ecosystem Integration

This document records **possible future integrations** between InfiltratorFS and
the wider Infiltrator software family. It is intentionally aspirational. It is
not a feature-completion checklist, not a release promise and not qualification
evidence.

The current priority remains making the native Linux filesystem fast, stable
and ordinary enough to run a real Linux installation without unusual mount
latency, write stalls, desktop integration failures, recovery surprises or
filesystem-specific babysitting. Ecosystem integration must not distract from
that baseline.

## Design rule

Applications must not be required to understand InfiltratorFS internals.

The preferred relationship is:

```text
InfiltratorFS exposes generic capability
        ↓
applications optionally use it
        ↓
real application workloads expose weaknesses
        ↓
filesystem implementation and qualification improve
        ↓
all capable applications benefit
```

Applications must continue to work correctly on other filesystems. When an
InfiltratorFS-specific capability is absent, applications should fall back to
ordinary operating-system behaviour rather than failing.

Prefer standard operating-system interfaces such as reflink, FIEMAP,
`fallocate`, xattrs, `fsync`, rename and normal file APIs wherever they
already express the required contract. Add an InfiltratorFS-specific interface
only when the operating system has no adequate generic representation.

Filesystem policy remains filesystem-owned. Applications may communicate
intent, but they must not prescribe block placement or depend on private
on-disk structures.

## Software Manager integration

A future Software/InfiltratorFS integration could use retained generations and
reflinks to make package operations safer and cheaper.

Possible workflow:

```text
Resolve transaction
      ↓
Create pre-change checkpoint
      ↓
Stage with reflinks where useful
      ↓
Apply package transaction
      ↓
Verify resulting package/system state
      ↓
Mark transaction successful
```

If verification fails, Software could offer a graphical rollback to the
pre-change generation when the affected system layout makes that safe.

Potential capabilities include:

- named pre-update checkpoints;
- graphical rollback of a failed system update;
- reflink-based package/staging trees;
- checkpoint identities recorded in package transaction history; and
- eventual coordination with InfiltratorFS whole-volume rollback once that
  capability is complete and qualified.

The filesystem must remain unaware of Debian, `.deb`, APT-compatible metadata
or Software-specific product semantics.

## System Monitor integration

System Monitor could become a first-party observability client for
InfiltratorFS.

Potential read-only telemetry includes:

- current committed generation;
- checkpoint publication latency;
- dirty/pending copy-on-write state;
- logical versus physical data usage;
- reflink/shared-data usage;
- compression ratio and compressed extent counts;
- fragmentation metrics;
- allocation/free-space topology summaries;
- writeback latency and backlog;
- scrub/check health;
- retained-generation/snapshot counts; and
- filesystem-specific error/recovery counters.

This would benefit users while also giving InfiltratorFS development a real
desktop tool for finding latency, writeback and checkpoint regressions.

Telemetry interfaces should be stable, bounded and read-only by default.

## Defragmenter integration

Defragmenter should not reverse-engineer InfiltratorFS metadata the way it must
for unrelated filesystems.

A future native management interface could expose generic administrative
operations such as:

- allocation/fragmentation maps;
- fragmentation metrics;
- online copy-on-write defragmentation;
- bounded extent relocation requests;
- scrub/check state;
- compression and sharing statistics;
- snapshot/history enumeration; and
- filesystem-health information.

Defragmenter would then act as a graphical administration client while
InfiltratorFS remains the sole owner of its format, safety rules and mutation
semantics.

## Application storage-intent hints

Applications sometimes know useful facts that the filesystem cannot infer
reliably.

A future optional hint mechanism could express broad intent such as:

- durable evidence/log data;
- temporary/disposable data;
- latency-sensitive data;
- archival/mostly-immutable data;
- streaming/sequential data; or
- already-compressed media.

The interface could use an xattr or another versioned generic policy API.

These are **hints, not commands**. An application may say that a file is
latency-sensitive or archival; InfiltratorFS still decides allocation,
compression, checkpoint and writeback policy.

No hint may weaken ordinary durability or correctness guarantees.

## Durable diagnostic evidence

LINK-family applications produce captures, diagnostic evidence and exported
reports where successful publication should have a clear durability meaning.

A future integration could combine Common's generic durable-publication
primitives with InfiltratorFS-specific strength underneath them:

```text
create temporary object
      ↓
write evidence
      ↓
fsync / integrity metadata
      ↓
atomic rename/publication
      ↓
durable parent publication
```

LINK remains filesystem-neutral; InfiltratorFS simply provides a strong
implementation of the generic durability contract.

## File history for applications

Retained generations could support application-visible previous-version
features without every application inventing a private backup-rotation format.

Potential consumers include:

- Character Manager documents and archives;
- Backyard Racer save files;
- configuration/state files;
- exported diagnostic evidence; and
- user-authored documents.

A future generic history API might support operations conceptually equivalent
to:

```text
list historical versions of object/path
open historical version read-only
restore selected historical version
```

The exact API is deliberately undefined until object-level restore/rollback is
implemented and qualified.

## Cheap development and CI workspaces

Reflinks and retained generations could make project builds and test
environments cheaper.

Possible uses include:

- reflinked throw-away build trees;
- snapshot/reflink test workspaces;
- fast reset after destructive tests;
- low-cost parallel experiment trees; and
- measuring logical versus unique physical build-space consumption.

Runner Monitor could eventually expose filesystem-backed runner workspace
metrics when such data is available, without making runners dependent on
InfiltratorFS.

## Persistent object identity

InfiltratorFS already has persistent 128-bit object identities. Selected
applications may eventually benefit from an optional object-identity API for
cases where paths are insufficient, for example:

- tracking an object across rename;
- associating history with an object after namespace changes;
- administration tools following an object during relocation; or
- correlating durable application metadata with the same persistent object.

Paths remain the normal application contract. Object identity should be an
advanced optional facility, never a requirement for ordinary file access.

## Application-driven qualification

The Infiltrator software family can become a real-world workload suite for
InfiltratorFS.

Representative workloads include:

- Software: package metadata, repository refresh, staging and durable
  transactions;
- Calculator/Calendar: small configuration and history updates;
- LINK/MBLINK/JAGLINK: append-heavy captures and durable evidence publication;
- Character Manager: database-style metadata plus larger visual assets;
- Backyard Racer: save-game replacement and asset streaming;
- Git/build trees: high metadata churn, parallel creation and temporary files;
- Repository/site generators: deterministic bulk publication; and
- System Monitor: observation of filesystem latency and pressure while those
  workloads run.

A future qualification gate could run selected application regression suites on
a native mounted InfiltratorFS volume and verify both application correctness
and filesystem integrity after the workload.

This is intentionally additional evidence, not a replacement for filesystem
unit, crash/recovery, mounted stress or destructive qualification.

## Common integration

Generic mechanisms discovered through these integrations should move into
Infiltratr Common only when they are truly product-neutral and at least as
correct as the strongest existing implementation.

Examples may include:

- capability discovery helpers;
- durable publication wrappers;
- stable history/identity value types;
- telemetry formatting; and
- portable fallback behaviour.

Filesystem semantics, package semantics, diagnostic semantics and
application-specific policy stay in their owning projects.

## Suggested sequence

These ideas should be considered only after the underlying filesystem reaches
the corresponding maturity.

1. **Native Linux baseline** — normal boot, mount, desktop, read/write,
   writeback, page-cache, latency, crash/recovery and sustained workload
   behaviour.
2. **Observability** — stable filesystem telemetry suitable for System Monitor
   and development diagnostics.
3. **Administrative interfaces** — Defragmenter/Manager integration over
   explicit safe native operations.
4. **History and checkpoint APIs** — object restore and whole-volume rollback
   after the underlying semantics are complete and qualified.
5. **Software transaction integration** — pre-update checkpoint and graphical
   rollback.
6. **Optional application hints/history/identity** — only where real workloads
   demonstrate value.
7. **Application workload qualification** — use the wider software family as a
   permanent real-world filesystem regression suite.

The intent is a coherent ecosystem in which InfiltratorFS makes applications
better and the applications, in turn, provide realistic pressure and evidence
that make InfiltratorFS better—without coupling either side to private
implementation details.
