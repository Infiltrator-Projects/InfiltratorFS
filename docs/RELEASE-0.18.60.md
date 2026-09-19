<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS 0.18.60

0.18.60 is the release line for the substantial native-Linux, performance,
correctness and maintainability work accumulated after the immutable 0.18.59
tag. The on-disk format remains 0.18.

The native driver now enforces a filesystem-wide execution budget of
`max(1, online logical CPUs - 1)` for scalable background and prepared work,
leaving one logical CPU for the rest of the operating system whenever possible.
Prepared writes, verified-read hashing, buffered writeback, orphan recovery and
idle publication use the bounded unbound CPU pool rather than concentrating
long-running work on ordinary per-CPU workers.

Large-volume behaviour was tightened substantially. Writable mount no longer
performs the complete crash-orphan catalogue walk synchronously. Recovery takes
a short catalogue snapshot, scans in bounded lock batches, re-resolves objects
before reclaim and fences candidates to the committed generation present when
the writable mount began. `getattr` uses cached persistent inode metadata
rather than entering the filesystem-wide writer rwsem.

Shared-extent ownership is now maintained by an incremental reference index.
Unlink/final eviction no longer rebuilds whole-volume ownership state for every
file, while exact fallback checks remain for ranges that may still be shared.
Reflink and snapshot safety therefore remain fail-closed without restoring the
old per-file catalogue scans.

The native data path also gains complete volatile object-index and directory
locators for repeated mutation, cross-call verified-read checksum caching and
the current readahead/writeback optimisations. Online defragmentation preserves
the exact VFS timestamps it is not semantically changing, hard-link ctime
coherence is maintained, and delayed native writes preserve persistent
mtime/ctime across remount.

Kernel implementation composition is smaller and more explicit. Shared
ownership, native locator caches and the persistent checksum store are compiled
Kbuild objects with private interfaces in `infiltratorfs_internal.h`, and the
same object list is reproduced by DKMS, Debian packaging, installers and native
qualification. The remaining textual RW composition is guarded by source-size,
alias and dependency policy so extracted subsystems cannot silently regress
back into the compositor.

Normal `fsck.infiltratorfs` is explicitly metadata-topology bounded: it checks
the committed structural graph and checksum metadata without turning an
ordinary fsck into a full payload/ownership scrub. Exhaustive data reading,
checksum recomputation and retained-generation traversal remain the explicit
`--scrub` path.

Userspace advances to Infiltratr Common 1.19.3 where the shared primitive is
genuinely product-neutral, while persistent-format policy, allocation,
transactions, snapshots/reflinks, IAC1 and kernel locking remain owned by
InfiltratorFS. Managed Linux desktop-integration packaging is also kept separate
from the core DKMS package and validated by the release artifact gate.

Publication of `v0.18.60` is fail-closed: the exact release commit must pass
Build and conformance, native Linux mounted qualification, Linux metadata/root
qualification and release-artifact assembly before the immutable tag and assets
can be created.
