<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS documentation map

This index defines the role and authority of repository documentation. The goal
is to keep design intent, byte-level specification, implementation status,
qualification evidence and historical investigation from becoming competing
sources of truth.

## Architecture decision record

- `DECISIONS.md` — durable cross-cutting architectural choices and their consequences. It does not replace `ON_DISK_FORMAT.md`, `ROADMAP.md` or `QUALIFICATION.md`.

## Normative and architectural documents

- `ON_DISK_FORMAT.md` — persistent Format 0.18 structural contract. Exact
  packed fields/constants remain authoritative in `include/infilfs/format.h`.
- `ARCHITECTURE.md` — architecture, invariants, ownership boundaries and
  failure semantics that are broader than byte layout.
- `SECURITY.md` — current security guarantees, threat boundaries and the
  planned portable principal/ACL architecture.
- `PLATFORM_ADAPTERS.md` — operating-system adapter responsibilities and
  cross-platform preservation rules.
- `COMPRESSION.md` — IAC1 and compressed-extent representation/selection
  contract.
- `FSCK-SCRUB-SEPARATION.md` — maintenance-command semantics and the boundary
  between structural checking and deep data verification.

## Status and evidence

- `ROADMAP.md` — the only authoritative feature-completion list.
- `QUALIFICATION.md` — the only authoritative exact-source qualification
  ledger.
- root `README.md` — concise project/product entry point; it may summarise but
  must not become a second roadmap or qualification ledger.

## Operational and specialist material

- `FORENSICS.md` — forensic scanner semantics and evidentiary limits.
- `kernel/README.md` — local native-Linux module build/ownership guidance.
- `IAC1-2026-HARDWARE-DESIGN.md` — performance-design constraints for modern
  hardware. Normative codec semantics remain in `COMPRESSION.md`.
- `INSPIRATIONS.md` — comparative design context and primary-reference
  starting points; it is neither a specification nor a status document.

## Historical and analytical records

Release notes and performance incident/handoff documents record what was
observed or changed at a particular point in development. They are informative,
not normative. A historical document must identify its baseline/source and must
not override the current format, architecture, roadmap or qualification ledger.

This category currently includes:

- `RELEASE-*.md`;
- `UNLINK-PERFORMANCE-HANDOFF.md`; and
- `IAC1-WRITE-PERFORMANCE-HANDOFF.md`.

## Documentation change discipline

1. Put each fact in the narrowest authoritative document and link to it rather
   than copying the same status/evidence elsewhere.
2. State invariants and externally observable contracts in present tense.
   Development chronology belongs in release/history material.
3. Code comments should explain non-obvious intent, ordering, ownership,
   concurrency, failure or format constraints. They should not narrate obvious
   statements or preserve obsolete architecture solely as commentary.
4. Claims about qualification must name an exact source and evidence class.
5. Historical measurements must retain workload/hardware/source context and
   must not be presented as current performance without remeasurement.
6. Pre-1.0 format history is informative only; current accepted-format rules are
   defined by the current format sources.
