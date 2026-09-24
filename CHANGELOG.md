# Changelog

This file records user-visible, compatibility, architecture and validation changes for InfiltratorFS.

## 0.18.85 — 2026-09-24
- Align the Linux Manager sidebar divider with the suite-wide 248 px desktop navigation width.
- Keep filesystem format, kernel driver, storage, encryption, namespace and maintenance behaviour unchanged.
- On-disk Format remains 0.18.

## 0.18.84 — 2026-09-24
- Remove the whole-volume object-index snapshot from ordinary quota admission. Single-parent files/directories now resolve project ownership through their bounded parent chain, while the expensive alias scan is retained only for multiply-linked/no-parent files that genuinely require it.
- Skip project ownership resolution entirely for user/group-only quota workloads when no project quota rule is active.
- Add a static scalability guard so normal quota admission cannot silently regress to whole-index project discovery.
- Keep the filesystem-wide N-1 CPU budget unchanged: cheap admission remains synchronous by design, while genuinely independent heavy filesystem work continues to use the shared N-1 execution budget.
- On-disk Format remains 0.18.

## 0.18.83 — 2026-09-24
- Align Manager secondary device metadata with the suite-wide 11 px supporting-text scale.
- Keep filesystem format, kernel driver, storage, encryption, namespace and maintenance behaviour unchanged.
- On-disk Format remains 0.18.

## 0.18.82 — 2026-09-24
- Align Manager device navigation side margins with the suite-wide 8 px navigation rhythm.
- Keep filesystem format, kernel driver, storage, encryption, namespace and maintenance behaviour unchanged.
- On-disk Format remains 0.18.

## 0.18.81 — 2026-09-24
- Align the Manager empty-state heading with the same 28 px publisher title scale as populated Manager pages.
- Keep filesystem format, kernel driver, storage, encryption, namespace and maintenance behaviour unchanged.
- On-disk Format remains 0.18.

## 0.18.80 — 2026-09-24
- Align the Manager hero heading with the 28 px publisher-wide desktop heading scale.
- Keep filesystem format, kernel driver, storage, encryption, namespace and maintenance behaviour unchanged.
- On-disk Format remains 0.18.

## 0.18.79 — 2026-09-24
- Fail native Linux object creation closed when the current uid/gid cannot be represented in the Format 0.18 POSIX compatibility fields, instead of silently substituting numeric zero/root ownership. File, directory and symlink creation now all propagate the mapping error before publication.
- Make authenticated encrypted storage safe for concurrent partial-block writes with logical-block stripe locks. Reads share the same stripe and flush fences every stripe, preventing lost read-modify-write updates and durability barriers from passing in-flight AEAD record replacement.
- Synchronize replicated-storage member-health state without serializing independent member I/O; quarantine decisions are now race-free while the existing degraded-write and split-brain fail-closed behaviour is preserved.
- Make the Windows Explorer bridge shutdown durability boundary observable and retryable. A failed final publication now returns its error and keeps pending bridge/volume state alive rather than tearing it down and silently discarding the transaction.
- Add deterministic concurrent encrypted-write qualification, storage-thread-safety policy coverage and native creator-identity regression guards.
- Align architecture documentation with the already-implemented portable security-object model.
- On-disk Format remains 0.18.

## 0.18.78 — 2026-09-24
- Complete named-stream integration across scalable object-index validation, paged/tree extent handling, ownership accounting, checksum graphs, deep scrub, shared-extent protection and compression metrics; large named metadata streams now pass the same structural and data-integrity rules as regular file-data objects.
- Correct live checkpoint-replica validation after publication by treating the serialized superblock checksum as a derived encoding field rather than comparing it against the intentionally unmodified in-memory checksum bytes. Fast checks now recognise all freshly published checkpoint replicas without requiring a reopen.
- Serialize synchronous Win32 seek-plus-I/O operations with an SRW lock so concurrent positional storage requests cannot redirect one another through a shared HANDLE file pointer.
- Harden replicated storage against stale-member reuse: members that miss writes/flushes remain quarantined for the mirror lifetime, post-reopen divergent replicas are detected as corruption rather than silently selected, and degraded writes remain fail-closed while healthy replicas continue receiving data.
- Propagate per-file optimizer failures to recursive and single-file callers so maintenance commands cannot report shell success after a failed optimisation.
- Retire the dormant legacy whole-device fsync implementation so only the active deferred native fsync path can be wired into the VFS.
- Remove unused deferred-transaction base-superblock state and its dead assignment.
- Strengthen the deferred namespace publication policy guard: create/mkdir must enter the shared native namespace transaction and must not flush or directly commit a standalone generation per object.
- Fail closed after deferred transaction publication errors: once publication fails, fsync, idle work and unmount may report/abandon the poisoned transaction but must never retry the same possibly-indeterminate checkpoint publication before remount recovery.
- Propagate threshold-triggered inline writeback publication failures into the page-cache error path instead of discarding them and falsely completing writeback successfully.
- Add regression guards for terminal failed-publication state and inline publication-error propagation.
- Retire the unused flush-before-create/mkdir/setattr data wrappers and their macro alias bridge; the deferred POSIX/native namespace path is now the only compiled data-layer route for those VFS mutations.
- Align the native maintainability guard with the reduced legacy alias bridge. On-disk Format remains 0.18.

## 0.18.77 — 2026-09-24
- Reduce small-file writeback contention exposed by the 0.18.76 live rsync forensic capture. Inline-file writeback now snapshots the current object under the topology read side, performs inline integrity verification and replacement SHA-256 work without holding the filesystem-wide writer, then acquires the writer only for complete-object revalidation and CoW/index publication.
- Preserve lost-update safety for transaction-private blocks that may be rewritten in place: publication re-reads and compares the complete verified 4 KiB object under the writer and retries from a fresh snapshot if another mutation changed it.
- Add a small-file scaling policy guard that requires digest preparation before writer acquisition and complete-object revalidation before publication.
- On-disk Format remains 0.18.

## 0.18.76 — 2026-09-23
- Remove the per-file buffered-write drain from explicit regular-file mtime updates without weakening timestamp correctness: POSIX metadata rewrite now keeps the topology writer lock through VFS mtime/ctime publication, so delayed writeback cannot overwrite a newer `touch`, `cp -p` or `rsync -a` timestamp. This restores asynchronous page-cache behaviour for metadata-preserving copy workloads instead of forcing every file through `filemap_write_and_wait()`.

- Advance development after publishing 0.18.75; on-disk Format remains 0.18.
- Correct APT publication verification so a source release does not fail merely because the central pull-based repository publisher has not run yet. An accepted immediate repository dispatch is still verified synchronously; otherwise publication is left to the authoritative scheduled central workflow.
- Preserve the exact Infiltratr Common 1.19.24 pin at `748e089ae175329471d4cf375522c44081371bd5`.

## 0.18.75 — 2026-09-22

- Fix the residual sustained-write publication stall isolated by the 0.18.74 live phase telemetry: transaction publication was spending almost all tail latency inside allocation-map publication while dependency draining had fallen to only tens of milliseconds.
- Replace per-range incremental free-extent cache insertion during transaction publication with one authoritative word-at-a-time rebuild from the post-publication allocation bitmap. This removes the fragmented-volume O(n²) scan/memmove path while preserving the exact bitmap as allocation authority.
- Add allocation-map subphase telemetry for preparation, retired-range journalling, bitmap/index reclamation, leaf writes and branch writes, including dirty-tree counts, so any remaining publication tail can be attributed without another broad kernel trace.
- Keep operation-level rollback on incremental extent-cache repair because those rollback sets are bounded; only large transaction publication uses the one-shot rebuild.
- On-disk Format remains 0.18.

## 0.18.74 — 2026-09-22

- Publish 0.18.74 as the sustained native-write stall recovery release after the 0.18.73 development cycle; on-disk Format remains 0.18.
- Advance the exact Infiltratr Common dependency from 1.19.23 to 1.19.24 at `748e089ae175329471d4cf375522c44081371bd5`. This Common release hardens graphics surface range/alias handling; InfiltratorFS does not consume that graphics API, so the pin changes no filesystem or on-disk semantics.
- Split the filesystem-wide N-1 CPU/workqueue policy from the VFS/checkpoint core into a dedicated compiled `infiltratorfs_cpu.o`, preserving the same hotplug-aware execution gate while reducing `infiltratorfs_core.c` coupling.
- Consolidate native shared-range ownership discovery, exact fallback and incremental multiplicity accounting in `infiltratorfs_shared_ownership.c`, removing the duplicate range-append helper from the namespace compositor and shrinking its ownership surface.
- Split Linux Manager storage discovery, protected-device filtering and mount observation into a dedicated internal module, leaving the GTK source responsible for presentation/workflow rather than device inventory policy.
- Fix a sustained native writeback stall reproduced by a live Linux Mint root-tree rsync: committed CoW dependencies are now submitted progressively instead of accumulating hundreds of MiB of dirty buffer heads until the 512 MiB transaction-publication boundary.
- Snapshot each bounded dirty-folio writeback cluster before native compression/CoW work and release the page-cache folio locks before entering potentially long transaction publication, preventing multi-second publication latency from propagating as multi-second VFS folio lock stalls.
- Add native page-cache folio migration support, including transfer of the independent pending-CoW accounting marker, eliminating the Linux compaction warning for an address-space implementation with writepages but no migrate_folio callback.
- Add per-phase slow-publication telemetry for allocation-map construction, dependency draining, the dependency durability barrier, checkpoint writes and the final publication barrier so any residual tail latency can be attributed to a concrete publication phase.
- Harden the progressive-I/O design so staged ranges are submitted only after the individual operation is committed into the deferred transaction; volatile reservations are never reusable while stale I/O to the same physical range can remain in flight.


## 0.18.72 — 2026-09-22

- Publish 0.18.72 directly after 0.18.70; 0.18.71 was used only as an intermediate development identity and was never published.
- Advance the exact Infiltratr Common dependency from 1.19.20 to 1.19.23 at `a9cf2957cffeefe6001830916b8a32c2ef58a551`; the intervening Common releases are backward-compatible hardening plus a temporal helper and do not alter InfiltratorFS persistent semantics.
- Mark optional case-folded directories roadmap-complete after portable cross-platform conformance plus mounted native VFS/dcache qualification and CLEAN scrub.
- Add and mark generic typed/reparse extension objects roadmap-complete: stable 128-bit type IDs, type versions, opaque SHA-256-protected payloads, generic reparse/preserve flags, portable set/get APIs, scrub/reference validation, native-reader recognition and reflink preservation.
- Preserve the qualified CoW durability and reflink-scaling hardening accumulated during 0.18.71/0.18.72 development.
- Retain Format 0.18; this release does not introduce a backward-compatibility promise before 1.0.

## 0.18.70 — 2026-09-22

- Advance development after publishing 0.18.69; on-disk Format remains 0.18.
- Retain the validated mount-state reuse and persistent native free-space index improvements made after 0.18.69.
- Back out the unqualified list/token allocation-reservation refactor after mounted quota, metadata, parallel-write and resize qualification exposed user-visible `EAGAIN`/`Resource temporarily unavailable` regressions; retain the previously qualified reservation implementation until a replacement passes the full native suite.

## 0.18.69 — 2026-09-21

- Replace serial 4 KiB allocated-run reads with bounded 4 MiB buffer-head batches submitted under a block plug before waiting, while retaining buffer-cache coherence and SHA-256 verification.
- Keep speculative single-block readahead only for the partial-read fallback so multi-block reads do not submit the same physical range twice.
- Add explicit modern Linux O_DIRECT admission and aligned native read/write dispatch while preserving the page cache for ordinary I/O.
- Replace transaction-publication sync_blockdev() with targeted writeback of the exact transaction-private CoW allocation set; retain synchronous checkpoint writes and the final device-cache flush as the crash-consistency boundary.
- Add executable O_DIRECT qualification and policy guards for batched reads, exact dependency writeback and the preserved durability barrier.
- Add a reproducible performance harness using incompressible data, single-process metadata tests, raw-device baselining and optional fio cross-checks without making fio mandatory.
- Keep the on-disk format at 0.18.

## 0.18.68 — 2026-09-21

- Make desktop integration mandatory for normal InfiltratorFS package installation so a system cannot stop at correct udev/UDisks identity while stock GNOME Disks still renders "Unknown (infiltratorfs 0.18)".
- Make the published native .run self-contained by embedding the qualified Ubuntu 24.04 / Linux Mint 22.x libblockdev, GNOME Disks and integration packages, installing them atomically with the locally compiled core package, then verifying package ownership and UDisks formatter capability.
- Gate release assembly and publication on the presence of that embedded desktop-integration bundle and on the core Debian package carrying a hard dependency on infiltratorfs-desktop-integration.

- Standardise Linux and Windows About presentation on the suite-wide System Monitor contract, including canonical Build, Website, Credits, Licence and Close semantics.
- Remove the Linux About-specific styling/tagline and replace the oversized Windows technical-information message with a compact native TaskDialog.


- Fix a Manager discovery deadlock where Inspect was disabled unless the target had already been identified as InfiltratorFS. Inspect is now available for any selected unmounted target, while Check, Scrub, Mount and filesystem-specific operations still require positive InfiltratorFS identification.
- Add an executable regression test proving an unknown unmounted target remains inspectable without weakening the safety gates on filesystem-specific maintenance.

## 0.18.66 — 2026-09-21

- Replace foreground portable-security scans with bounded persistent lookup: resolvable bindings gain deterministic secondary-index objects in the existing object index, immutable descriptors use content-derived collision-slot IDs for direct reuse, opaque bindings remain non-resolvable, and ACL detach/unlink becomes independent of total namespace size with reclamation deferred to graph-tracing maintenance.
- Correct the portable-security identity model before further feature work: advance the development payload contract to version 2, scope POSIX numeric bindings by a 128-bit identity authority, require canonical binary Windows SIDs, make opaque bindings preservation-only, reserve implicit OWNER/GROUP/EVERYONE/CREATOR_OWNER/CREATOR_GROUP principals, and specify graph-traced rather than foreground full-namespace reclamation.
- Harden the portable ACL evaluator and inheritance engine to reject malformed in-memory descriptors fail-closed, preserve ordered allow/deny decisions, and canonicalise the file-only/no-propagate directory inheritance edge case.
- Redesign the development portable-security model before format freeze: principal identities become volume-level indexed objects with multiple platform bindings, security descriptors become shareable owner/group + ordered-ACL objects, descriptor reachability is graph-derived rather than persistent-refcount based, and large ACLs are permitted to page beyond one metadata block.
- Implement versioned portable security objects with stable 128-bit principals, typed POSIX/Windows/opaque identity bindings, ordered allow/deny ACL entries, inheritance flags, transactional attachment/replacement/removal, native-reader recognition and bidirectional scrub validation.
- Mark the Windows SID/security-descriptor mapping policy roadmap-complete after exact-source cross-platform conformance and Windows Explorer bridge qualification.
- Define an executable Windows security-descriptor mapping policy: SID bindings remain adapter-local identities, DACL access masks project onto portable file/directory/metadata/administrative rights, generic masks are expanded explicitly, and Windows-only request/audit semantics cannot silently widen portable access.
- Mark the portable access-right vocabulary and Linux UID/GID/mode/POSIX ACL projection policy as roadmap-complete after exact-source cross-platform conformance plus mounted ACL/remount qualification.
- Introduce a platform-neutral 64-bit access-right vocabulary for future portable ACL/security objects without reusing Linux or Windows ABI bit values.
- Define executable Linux/POSIX mode and ACL projection helpers, including distinct regular-file and directory semantics while keeping UID/GID values adapter-local rather than portable principal identities.
- Mark the filesystem-wide native Linux N-1 concurrency budget as roadmap-complete after mounted real-write qualification reached budget=3 with prepared-write peak=3 and the complete exact-source CI/native suites passed.
- Add mounted N-1 CPU-budget qualification with real prepared-write concurrency telemetry, proving CPU-heavy write preparation can fill the filesystem budget before short serialized publication.
- Mark the cross-platform removable-volume filename profile as roadmap-complete after exact-source portable, mounted native, metadata, root-volume and resize qualification.
- Add an optional persistent removable-volume filename profile with cross-platform component constraints, portable/native enforcement, fail-closed graph validation, mkfs selection, and mounted qualification.

- Mark the versioned Unicode namespace policy and deterministic unambiguous fsck repair as roadmap-complete after exact-source conformance, mounted metadata and root-volume qualification.
- Define and persist Unicode namespace policy v1: valid UTF-8 is preserved byte-for-byte with no implicit normalization, and both portable/native readers reject current-format media that omits the explicit policy bit.
- Strengthen deterministic fsck repair qualification: preen proves recovery from either one or two damaged checkpoint replicas when the surviving graph is authoritative, while non-checkpoint metadata corruption is verified fail-closed and byte-preserving.
- Allow the release APT verification gate a bounded eight-plus-minute propagation window so the five-minute scheduled repository importer and GitHub Pages deployment cannot race the previous seven-minute verifier cutoff.

## 0.18.65 — 2026-09-20

- Modernise the native Linux page-cache read path on Linux 7.0 and newer by delegating folio read/readahead state management to iomap while retaining InfiltratorFS's verified native transport for sparse extents, compression and SHA-256 integrity.
- Keep pre-7.0 kernels on the already-qualified direct-bvec page-cache implementation rather than introducing an emulated iomap compatibility layer.
- Reserve folio private state for iomap and track InfiltratorFS pending-CoW accounting independently with the auxiliary folio flag.
- Fix native writeback accounting so nr_to_write is decremented by base-page count independently of cluster batching, including large-folio fallback.
- Keep page-cache reads and writeback zero-copy at the folio/native-iterator boundary and prohibit generic iomap bio reads that would bypass filesystem verification.
- Add an exact upstream Linux 7.0 compile gate for the out-of-tree module and prove the iomap symbols consumed by InfiltratorFS are exported for modules.
- Move the extent-pointer-tree metadata-page workspace off the kernel stack after the upstream Linux 7.0 build exposed a 4 KiB stack frame.

- Give the Linux Manager its own project-owned application identity icon: the same graphite tile and canonical `#00ADEF` filesystem glyph used by the Software Centre, installed once for hicolor and as the `infiltratorfs` Mint app-install alias; UDisks/device surfaces deliberately keep the platform drive icon because they represent volumes rather than the application.

- Complete the shared Manager behaviour contract across Linux and Windows: action success copy, target/action enablement policy and GUI capacity formatting now come from one platform-neutral module, leaving GTK/Win32 storage, mount and native-file-manager mechanics in their adapters.
- Match Windows capacity presentation to Linux by using the same Common auto-scaling disk-capacity contract rather than a Windows-only fixed-GiB formatter.
- Update the UI/Common policy gates to assert the shared Manager capacity contract instead of the superseded Windows-only fixed-GiB implementation.
- Move common target-state copy (ready/mounted/unmounted/unknown filesystem), image naming, basic header controls and filesystem labels into the shared Manager contract so GTK and Win32 cannot silently drift in wording.

- Begin converging the Linux and Windows Managers onto one shared application/presentation contract: identical core wording, maintenance actions, structure labels and semantic action roles now come from one C module while GTK/Win32 remain thin native presentation adapters.
- Advance the development source to 0.18.65 so post-0.18.64 cross-platform Manager work cannot be mistaken for the immutable published release.
- Rebuild the Windows Manager around the same application structure as Linux: storage sidebar, shared hero/mount controls, Overview/Files pages, capacity/filesystem/status cards, volume information, identical four maintenance actions, destructive-format zone and activity log. Windows-only file transfer remains an adapter capability on the Files page rather than defining a separate application.
- Add Windows parity for Create Image, fast structural Check and Forensic Scan through the same portable-core APIs used by the Linux tooling, while retaining the ProjFS Explorer bridge as the Windows-only mount adapter.
- Apply the full Common 1.19.10 Night presentation roles to the Win32 Manager, including connection surface, headings, summaries, kickers, detail/note text and semantic success/warning/info/fault/accent states.

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
