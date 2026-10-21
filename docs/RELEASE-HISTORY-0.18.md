<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS 0.18 release history

These are the original, historically scoped technical notes for selected 0.18 releases, consolidated without deleting their findings. They describe the state of their respective released sources, not the current Format 0.19 design or qualification evidence. Current architecture, status and evidence remain in `ARCHITECTURE.md`, `ROADMAP.md` and `QUALIFICATION.md`.

## InfiltratorFS 0.18.56

This release carries the Linux native buffered clustered writeback repair introduced after 0.18.55.

Normal Linux writes now enter the page cache instead of forcing the calling userspace thread to synchronously perform compression, integrity and CoW publication work. Dirty folios are written back in bounded contiguous batches while preserving the existing verified CoW data path. The repair also separates visible dirty EOF from persisted EOF, integrates quota reservation with buffered writes, removes the steady-state SHA-256 transform lookup mutex, and bounds IAC1 predictor classification work.

Qualification of the buffered path also exposed and repaired two quota regressions before release: whole-request quota admission again rejects writes that would cross a limit, and cross-project reparenting accounts for dirty page-cache file sizes instead of stale persisted sizes.

Free-space reporting now includes dirty page-cache CoW demand before physical allocation. The estimate is released after successful writeback or full invalidation; repeated dirtying is counted once, and failed writeback retains the demand. This preserves asynchronous buffered writes while making statfs reflect pending data.

The on-disk format remains 0.18.
Explicit file timestamp changes now drain earlier buffered writes before updating the persisted metadata, preserving `touch` and `cp -p` modification times across delayed writeback and remount.

---

## InfiltratorFS 0.18.57

Fix Linux native read-only to read-write remount initialization, preserve mount policy across remounts, and synchronize writes before read-only transitions. Add native remount regression qualification.

Batch metadata reads during orphan discovery and remove redundant object-index lookups while retaining the full recovery scan and validation. Report orphan-scan duration in the kernel log.

Preserve the mounted filesystem root owner, permissions and ACLs in the desktop manager helper instead of changing them to the invoking user.

The on-disk format remains 0.18. Qualification includes native remount cycles, open-unlinked recovery and clean filesystem scrubs. These repairs do not resolve all large-volume performance problems: a million-file clone still takes about 23 seconds for orphan discovery, and boot and runtime stalls remain under investigation. This release does not claim those stalls are fixed.

---

## InfiltratorFS 0.18.58

Advance the exact shared Libraries dependency from 1.11.0 to 1.19.2 at commit `44409af17c89b6ece6b4bcb2c0c133213c695c23`.

The Common submodule, CMake version/commit contract and public project status now agree on the same immutable Common release. This keeps InfiltratorFS on the current shared foundation without changing the InfiltratorFS on-disk Format 0.18 contract.

No on-disk format migration is introduced by this release.

---

## InfiltratorFS 0.18.59

**Published:** 2026-09-18.

Use Libraries 1.19.2 more deeply only where it improves InfiltratorFS
itself.

This release removes duplicate userspace numeric and binary-size parsing from
the resize, quota, optimize, mkfs and image tools, replaces the image tool's
hand-written exact-read loop with Common's EINTR-safe exact I/O helper, and uses
Common's overflow-safe dynamic-array reserve primitive for the portable
free-extent index, transaction allocation/deferred journals and snapshot
traversal state.

Filesystem-specific code remains local: the Linux kernel module, allocation
policy, copy-on-write machinery, persistent structures, integrity/checksum paths
and compression are unchanged. The on-disk Format 0.18 contract is unchanged.

---

## InfiltratorFS 0.18.60

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

Userspace advances to Libraries 1.19.3 where the shared primitive is
genuinely product-neutral, while persistent-format policy, allocation,
transactions, snapshots/reflinks, IAC1 and kernel locking remain owned by
InfiltratorFS. Managed Linux desktop-integration packaging is also kept separate
from the core DKMS package and validated by the release artifact gate.

The ABI-matched Ubuntu 24.04 / Linux Mint 22.x libblockdev and GNOME Disks
replacement packages were rebuilt from the current patch/build contract and
passed the installed real-stack qualification before this release commit.

Publication of `v0.18.60` is fail-closed: the exact release commit must pass
Build and conformance, native Linux mounted qualification, Linux metadata/root
qualification and release-artifact assembly before the immutable tag and assets
can be created.

---

## InfiltratorFS 0.18.61

0.18.61 fixes the Linux GTK3 manager headerbar contrast regression visible in
the System/Night theme. The Common palette itself was correct; the local GTK CSS
cascade was not.

The manager previously assigned the normal text colour to the universal `*`
selector. GTK renders button text and symbolic icons as child CSS nodes, so
those children received that explicit colour instead of inheriting each
button's foreground. In the night palette this produced pale labels/icons on
the deliberately pale button surface, including the theme selector and window
controls.

The fix removes the universal foreground assignment, explicitly carries each
button state's foreground to its label/icon children, keeps header icons fully
opaque, and strengthens the title/subtitle typography. A regression policy now
rejects reintroducing a foreground on the universal GTK selector.

No filesystem semantics or on-disk structures change. The on-disk format
remains 0.18.

---

## InfiltratorFS 0.18.69

This release addresses the real-machine I/O bottlenecks exposed by the native
performance qualification on a fast NVMe device.

The native verified read path now submits contiguous allocated runs in bounded
4 MiB buffer-head batches before waiting, allowing the Linux block layer and
NVMe device to carry useful queue depth without bypassing InfiltratorFS SHA-256
verification. The single-block path retains bounded speculative readahead.

Linux O_DIRECT is explicitly admitted through FMODE_CAN_ODIRECT and routed
through aligned native verified read/write callbacks. Ordinary read/write
continues to use the page cache.

Transaction publication no longer calls sync_blockdev() for every fsync. After
the replacement allocation tree is staged, publication drains only the
transaction-private CoW allocation set, writes checkpoint replicas
synchronously, and retains the final block-device cache flush as the
stable-media crash-consistency boundary.

The performance harness now avoids the earlier measurement errors: metadata
operations run in one process instead of thousands of helper processes,
throughput uses incompressible input, results are returned to the invoking
user, the raw-device control is included, and fio is an optional cross-check
rather than a requirement.

The on-disk format remains 0.18.

---

## InfiltratorFS 0.18.74

### Native Linux sustained-write stall recovery

0.18.74 addresses a system-wide latency failure reproduced by mirroring a live Linux Mint root filesystem onto a populated InfiltratorFS volume with rsync.

The captured workload showed repeated deferred-transaction publications lasting roughly 6–20 seconds, high full-I/O PSI, InfiltratorFS and rsync tasks in uninterruptible sleep, and unrelated interactive display/I2C paths stalling at the same time even though the NVMe device itself was not continuously saturated.

This release changes the native Linux writeback path in four related ways:

- freshly committed CoW dependency ranges are submitted progressively instead of leaving hundreds of MiB of dirty buffer heads to be first issued at the 512 MiB transaction-publication boundary;
- dirty page-cache clusters are snapshotted into bounded staging memory and their folio locks are released before native compression, CoW metadata work, or checkpoint publication can block for a long interval;
- page-cache folio migration is implemented, including preservation of InfiltratorFS's independent pending-CoW accounting marker, removing the Linux compaction warning for a writepages filesystem without migrate_folio; and
- slow transaction publication now reports separate timings for allocation-map construction, dependency draining, the dependency durability flush, checkpoint writes, and the final publication flush.

Progressive dependency submission occurs only after an individual operation is committed into the active deferred transaction. Blocks that are still only volatile allocation reservations are never submitted, preventing rollback/reuse from racing stale in-flight I/O.

The stable-media ordering contract is unchanged: transaction publication still waits for every CoW dependency, performs the dependency durability barrier, writes checkpoint replicas, and performs the final publication barrier before the new generation becomes authoritative.

### Compatibility

- On-disk format remains **0.18**.
- Existing Format 0.18 volumes remain the development target.
- Compression and SHA-256 integrity remain enabled and unchanged in meaning.
- No compatibility promise is introduced before 1.0.

### Validation

The regression source was a real sustained Linux Mint migration rather than a short synthetic throughput test. Exact-source native kernel, metadata, root-volume, resize, userspace and release qualification remain required before immutable publication.
