# Changelog

This file records user-visible, compatibility, architecture and validation changes for InfiltratorFS.

## Unreleased

- Begin converging the Linux and Windows Managers onto one shared application/presentation contract: identical core wording, maintenance actions, structure labels and semantic action roles now come from one C module while GTK/Win32 remain thin native presentation adapters.
- Advance the development source to 0.18.65 so post-0.18.64 cross-platform Manager work cannot be mistaken for the immutable published release.

- Advance to Infiltratr Common 1.19.10 and consume the complete Linux MBLINK-derived appearance roles in the native Manager, including titlebar, connection/status surface, heading/summary/kicker/detail/note text, selected-summary, accent-hover/foreground and muted state-border colours.
- Keep filesystem-specific semantic colouring on top of those canonical roles: success for mounted/check, warning for offline/scrub, fault for destructive formatting, information for inspection and the canonical blue accent for filesystem/forensic operations, without changing layout or behaviour.

## 0.18.63 — 2026-09-19

- Complete native Linux mutation of scalable extent-pointer trees, including promotion beyond the direct page-head ceiling and safe demotion when extent metadata shrinks.
- Avoid a second whole-device writeback walk during transaction publication: stage dependencies durably, synchronously submit the three checkpoint buffers, then issue the device-cache flush.
- Avoid whole-device `sync_blockdev()` work for clean `fsync()` calls when no native transaction exists to publish.
- Add slow-path diagnostics for checkpoint selection, writable mount initialisation, checkpoint healing, quota reconstruction, snapshot retention-map construction and transaction publication.
- Report checkpoint-replica divergence when it forces expensive deep graph validation so recovery-time stalls are directly attributable in the kernel log.
- Preserve crash-safe checkpoint ordering and fail-closed write poisoning on indeterminate durability failures.
- Advance userspace to Infiltratr Common 1.19.8 and consume its canonical typography, design metrics, font provenance, project identity, formatting and POSIX path contracts where they fit without moving filesystem semantics into Common.
- Fix the native Manager's failed-command capture leak exposed by ASan/LeakSanitizer.
- Forensically audit Common 1.19.8 call sites: use checked shared arithmetic for extent/allocation and Win32 growth paths, and use the generic Common scaler for Windows capacity text while preserving the established two-decimal GiB presentation.
- Add a Common-usage policy gate so these call-site choices cannot silently drift back to local duplicate arithmetic or capacity scaling.
- Preserve the established fixed two-decimal MiB defrag output and fixed two-decimal GiB compression metrics through Common's generic scaler; do not substitute the auto-scaling disk-capacity helper where the command-line unit itself is part of the interface.
- Complete a second Common 1.19.8 call-site pass across compression, attribute accounting and scalable directory metadata so persistent range/offset/size arithmetic consistently uses the shared checked primitives without changing filesystem semantics or user-visible units.
- Route mkfs block I/O through Common's uint64 exact positioned-I/O contract without pre-narrowing offsets to off_t, and check block-to-byte/bitmap-coverage multiplication before issuing I/O.

## 0.18.62 — 2026-09-19

- Fix the native prepared-append queue-failure path so a failed workqueue enqueue cannot strand a completion waiter.
- Replace the fixed direct extent-page ceiling with scalable extent-pointer trees while retaining the compact direct representation for smaller extent maps.
- Retain snapshots by immutable allocation-tree root instead of copying a whole-volume allocation bitmap at snapshot creation.
- Page the snapshot catalogue beyond the former single-object 26-record capacity and validate paged catalogue records in both portable and native paths.
- Accept snapshot and extent-index metadata pages in the central portable finalizer and account for paged snapshot-catalogue blocks in exhaustive ownership validation.
- Ship the extracted native extent-tree component in DKMS qualification and Linux packages.
- Remove obsolete one-shot source-rewrite helpers left behind by earlier refactors.
- Keep the development on-disk format identifier at 0.18 while replacing the prior development representation; backward compatibility is not claimed before format freeze.


## 0.18.61 — 2026-09-19

- Fix GTK3 headerbar contrast by allowing button child labels/icons to inherit the active button foreground instead of forcing the global text colour onto every child CSS node.
- Strengthen InfiltratorFS title/subtitle readability and keep header controls fully opaque in System/Day/Night themes.
- Add a UI policy guard so a universal foreground cannot silently reintroduce the contrast regression.

## 0.18.60 — 2026-09-19

- Enforce the native Linux N-1 logical-CPU execution budget for scalable background/prepared work.
- Move crash-orphan recovery off the synchronous mount path and bound its topology-lock hold times.
- Maintain shared-extent ownership incrementally so unlink/final eviction avoid repeated whole-catalogue scans.
- Add native object/directory locator caches, verified-read checksum caching and further write/readahead scaling work.
- Preserve VFS timestamps across defrag, hard links, delayed writes and remount.
- Extract shared ownership, locator-cache and persistent checksum-store logic into explicit compiled kernel objects.
- Keep ordinary fsck structural and topology-bounded while reserving exhaustive payload verification for `--scrub`.
- Advance the shared userspace foundation to Infiltratr Common 1.19.3.
- Keep the on-disk format at 0.18.

## 0.18.59 — 2026-09-18

- Consolidate product-neutral userspace parsing, exact I/O and checked dynamic-array growth on Infiltratr Common 1.19.2.
- Keep filesystem semantics and the on-disk Format 0.18 contract unchanged.

## Historical source

Git tags and GitHub Releases remain authoritative for exact historical source and release assets. Existing specialist release notes remain valid where present.

## Policy

Record behaviour changes, fixes, compatibility changes and support-boundary changes. Do not invent historical detail that cannot be tied to a release or commit.
