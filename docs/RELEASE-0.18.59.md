<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS 0.18.59

**Status: draft / not yet published.**

0.18.59 deepens use of Infiltratr Common 1.19.2 only where the shared primitive
reduces duplicate generic code or makes an existing boundary safer. It does not
move filesystem semantics into Common.

The consolidation covers:

- userspace numeric, range and binary-size parsing in administration/image tools;
- exact EINTR-safe userspace I/O and portable POSIX path canonicalisation;
- UTF-8 validation and endian byte-load/store primitives;
- checked integer/size arithmetic for generation growth, extent/range validation,
  bitmap/layout sizing, Windows partition ranges and dynamic allocations;
- overflow-safe dynamic-array reserve for runtime indexes, transaction journals
  and snapshot traversal state;
- shared percentage/reporting helpers where their units and semantics match the
  InfiltratorFS contract; and
- Windows bridge allocation sizing without changing ProjFS behaviour.

The work deliberately leaves filesystem-specific responsibilities local:
allocation and placement policy, copy-on-write publication, persistent-format
validation semantics, checkpoint/recovery policy, snapshots/reflinks, IAC1,
kernel-only code, realtime timestamp policy and platform-specific locking remain
owned by InfiltratorFS.

No on-disk format migration is introduced. The on-disk format remains 0.18.
