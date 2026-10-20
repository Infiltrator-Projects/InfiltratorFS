# Changelog

## 0.19.5 — 2026-10-20

- Complete verified Linux page-cache folios atomically using the standard kernel read-completion API, including correct fail-closed completion after unsuccessful verification.
- Finish queued readahead folios before taking the single-folio recovery path if batching cannot represent a folio.
- Exercise 24 independent cold remounts per native quota/ACL/data qualification; leave the original first-byte read failure test enabled. Format 0.19 on-disk structures remain unchanged.


## 0.19.4 — 2026-10-20

- Record the VFS folio state when a buffered read returns an error without clearing the mapping error or bypassing data verification.
- Keep page-cache diagnostics in their owning module and reduce the native RW compositor below its enforced maintenance limit.
- Advance immutable release identity from the previous diagnostic/review commit.


## 0.19.3 — 2026-10-20

- Reject output handles that alias input storage members in the portable mirror and authenticated-encryption constructors, preserving caller ownership on invalid requests rather than clearing live source handles.
- Centralize mirror-wrapper allocation teardown while retaining its explicit ownership-transfer boundary. Defer comparison-buffer allocation until a second healthy replica actually requires verification; preserve replica disagreement rejection and read failover.
- Add regression coverage for mirrored and encrypted storage-handle aliasing and document the constructor ownership precondition. Persistent Format 0.19 bytes and durability protocol are unchanged.

## 0.19.2 — 2026-10-19

- Align maintained format, security, compression, forensic, kernel and architectural documentation with the already-implemented Format 0.19. Preserve historical Format 0.18 decision rationale while stating its resolution and outstanding physical-media qualification accurately.
- Correct the directory-tree specification so it explicitly distinguishes the default byte-exact namespace from the optional ASCII case-fold policy.
- Strengthen the documentation authority check against the actual version constants in the canonical packed-format header, without adding a second format-version authority. Disk bytes, public APIs and filesystem execution paths are unchanged.

## 0.19.1 — 2026-10-18

- Fix interrupted resize recovery when locator B was already damaged: retain the old primary geometry map while publishing the two new remote checkpoints, then publish the primary and final locator. Enforce this backup-first relocation order in the shared portable/native publisher. Existing Format 0.19 media remains compatible.
- Extend actual-writer fault qualification to latent damage of either locator after writable open, every grow/shrink publication boundary, and interrupted checkpoint/locator healing. Recover through the normal API, verify complete data and graphs, require repaired redundancy before resuming writes, and enforce reported physical capacity in the test backend.
- Remove a redundant portable repair flush after all repaired checkpoints were already individually flushed.
- Correct native Windows checkpoint-prefix packing and assert canonical disk offsets at WDK compile time. Mounted native-Windows qualification remains separately tracked.

## 0.19.0 — 2026-10-18

- Complete Format 0.19 as the current testing format: normal portable/Linux formatting, writable opens/mounts/remounts, relocation-aware grow/shrink, checkpoint/locator repair, structural fsck and deep scrub. Superseded development formats are rejected.
- Share one durability protocol between portable and native writers: flush dependencies, publish each checkpoint with its own barrier, and stage relocation through locator A and finally locator B. Poison writes after ambiguous publication and preserve valid survivors during repair; ordinary transactions never rewrite locators.
- Discover both fixed maps and an independent primary map with a finite nine-candidate bound, recover relocated backups after block-zero loss, preserve I/O errors and reject reserved-block aliases. Fix writable feature admission and reject unaligned formatting before any writes.
- Add actual-writer fault qualification (148 transaction and 128 resize cases), default CLI repair tests, and native VM write/remount/resize/physical-image-shrink/recovery/repair qualification. Physical NVMe performance, hardware power-loss/endurance and mounted native-Windows qualification remain separate test work.

## 0.18.143 — 2026-10-18

- Extend experimental writable Format 0.19 regular-image qualification across separately injected I/O failures at each of the three checkpoint durability flushes. Require immediate writer poisoning, no further writes after failure, unchanged fixed recovery locators, full committed-graph recovery and a verified read-only reopen for every case.
- Distinguish simulated flush error handling from full power-loss and torn-write qualification; physical devices and native writable 0.19 remain disabled. Production disk Format remains 0.18.

## 0.18.142 — 2026-10-18

- Reject portable Format 0.19 recovery candidate sets that combine two individually checksum-valid locator records claiming different filesystem UUIDs, matching the native Linux fail-closed identity rule.
- Add both isolated locator-candidate and full committed-graph image tests with a forged, CRC-valid second UUID, then confirm the restored original locator recovers. No on-disk format migration or physical-device writes.
- Production persistent Format remains 0.18; Format 0.19 writable native mounts and geometry-changing operations remain prohibited until complete crash-safety qualification.

## 0.18.141 — 2026-10-18

- Require all exact-source native kernel, root-volume and Linux metadata release prerequisites on every `main` push, including CI-only corrections, preventing path-filtered revisions from deadlocking immutable publication.
- Update native kernel CI's exact userspace tool list to build the POSIX 0.19 fixture writer before native loopback tests; keep the fixture and kernel module on the same source revision.
- Extend native Linux Format 0.19 disposable loopback qualification from an empty bootstrap directory to a real POSIX-generated, multi-generation 256 KiB file. Require byte-exact namespace and data reads on intact, block-zero-lost, one-locator-lost, and primary-only recovery mounts.
- Continue rejecting native Format 0.19 read-write mounts and read-write remounts. Use only disposable regular-file test images; no physical device formatting, native 0.19 writes, resize or repair is enabled.
- Production persistent Format remains 0.18.

## 0.18.140 — 2026-10-18

- Complete experimental read-only fast structural fsck's Format 0.19 redundancy accounting: authenticate both permanent locator records against the selected committed geometry and UUID, while permitting locator generation hints to lag ordinary writes.
- Report recovery-locator health separately from checkpoint replicas. A recovery from block zero may be readable and pass deep scrub but must not report CLEAN fast structural fsck when its independent locators are damaged.
- Extend regular-file regression tests for 2/2 healthy locator records and 0/2 degraded locator records without any modifications during fsck. Production persistent Format remains 0.18; no 0.19 physical writing, native RW or resize is enabled.

## 0.18.139 — 2026-10-18

- Implement experimental read-only **fast structural fsck** on Format 0.19 regular images (`fsck.infiltratorfs -n --experimental-019-image <file>`), separate from the existing deep scrub. Validate actual 0.19 checkpoint replica CRCs, index, allocation and namespace metadata without allowing writes or repair.
- Correct shared checkpoint comparison to explicitly authenticate Format 0.19 superblocks while preserving the strict Format 0.18 production decoder.
- Qualify clean and post-write 0.19 image checks, deliberately degraded 2/3 checkpoint reporting, restored integrity, no-write SHA-256 invariants and refusal of repair/default write-capable modes.
- Disk Format 0.18 remains the production default. Experimental 0.19 native write, locator relocation/resize, repair and physical-media qualification remain incomplete.

## 0.18.138 — 2026-10-18

- Add real POSIX-backend regular-image Format 0.19 write qualification: create a file, write a deterministic 256-KiB payload, durably sync via operating-system fsync, close, read-only reopen and byte-compare the data, run deep metadata/data scrub, and reopen the healthy image writable.
- Extend the end-to-end experimental mkfs/fsck image test to use the production POSIX storage adapter, not only the in-memory fixture. No block devices are opened in experimental writable mode.
- Native 0.19 mounting remains read-only; crash-safe geometry relocation and NVMe qualification are still unfinished.

## 0.18.137 — 2026-10-18

- Expose the guarded portable Format 0.19 formatter through `mkfs.infilfs --experimental-019-image --force <existing-regular-image>`. It requires explicit destructive consent, preserves production default Format 0.18, and refuses physical block devices even with force.
- Run the experimental image-only `mkfs` and `fsck --scrub -n` together in actual Linux CLI qualification; test missing-force refusal and read-only recovery through a surviving primary checkpoint.
- Normal filesystem formatting/mounting and physical-device qualification are unchanged. Writable 0.19 remains restricted to the portable disposable-image API.

## 0.18.136 — 2026-10-18

- Add read-only experimental Format 0.19 regular-image deep scrub to fsck.infiltratorfs using `--scrub -n --experimental-019-image`. Ordinary fsck/repair remain restricted to Format 0.18; device targets and experimental online/repair modes are refused.
- Qualify disposable image scrub, recovery from two damaged locator pages using a validated primary, and fail-closed behaviour when primary and both locators are corrupted. No changes to normal or physical block-device paths.
- Writable Format 0.19 remains experimental image-only; native writable, crash-safe resize and real physical qualification are outstanding.

## 0.18.135 — 2026-10-18

- Harden experimental writable Format 0.19 image qualification with actual checkpoint flush counters and injected failure after primary publication. Assert the writer reports uncertainty and stays write-poisoned until reopened, and the committed graph remains recoverable read-only.
- Refuse experimental 0.19 writable open when either discovery locator is corrupt, a checkpoint replica is unreadable, or storage identifies a physical device; all rejections preserve the caller-owned handle and write zero media bytes.
- Check that each of the three checkpoint replicas is durably flushed separately during ordinary portable 0.19 transactions. On-disk production Format 0.18 and native writable lockouts are unchanged.

## 0.18.134 — 2026-10-18

- Implement a strictly gated experimental Format 0.19 **writable regular-image** open path: require matching valid locators, three identical readable checkpoint replicas and full committed-graph validation before enabling portable CoW writes. Physical devices and native writable mounting remain disabled.
- Persist 0.19 secondary checkpoints individually and poison the session after uncertain replica durability. Refuse Format 0.19 resizing until the separate crash-safe locator relocation protocol is complete.
- Test real portable create/write/sync, unchanged locator pages on same-geometry commits, full graph validation and fresh read-only reopen of committed data in disposable image memory.
- Production on-disk Format remains 0.18. This is not physical-media qualification or permission to reformat NVMe.

## 0.18.133 — 2026-10-18

- Update the 0.19 fault-injection conformance matrix to distinguish recoverable double-locator read failure (valid block-zero checkpoint still exists) from total discovery loss (both locators and block zero return I/O errors).
- Preserve media I/O error reporting when all independent discovery sources fail, with portable and native recovery remaining read-only test features. Production disk Format remains 0.18.

## 0.18.132 — 2026-10-18

- Recover read-only Format 0.19 test images after loss of both discovery locators when block zero is independently CRC-valid and explicitly identifies valid checkpoint geometry. Portable and native Linux readers use the primary's bounded checkpoint map; no speculative physical midpoint/end scan.
- Retain full committed-graph validation, reserved locator-block checks, and all native writable mount/physical formatting restrictions. Add disposable memory-image and native loop-mounted regression tests for primary-only recovery.
- Production persistent Format remains 0.18; 0.19 writable publication, crash-safe resize and physical qualification are still outstanding.

## 0.18.131 — 2026-10-18

- Continue portable Format 0.19 recovery after an individual locator or checkpoint read failure when another independently validated recovery copy survives. Preserve the first operational I/O error if none can be recovered, rather than reporting false corruption.
- Qualify single-locator and multi-checkpoint unreadable-page cases, both locators unreadable, and no media writes during read-only recovery.
- This is a portable recovery hardening change only. Production disk Format 0.18 is unchanged; Format 0.19 native writes, resize and physical formatting remain disabled pending qualification.

## 0.18.130 — 2026-10-18

- Strengthen experimental Format 0.19 native read-only qualification to use direct Linux mount/remount syscalls, avoiding util-linux's automatic read-only retry after a refused RW mount.
- Require raw RW mount attempts to return EROFS, and reject any successful writable remount; preserve strict native 0.19 writer lockout and the disposable-loop-only test boundary.
- This is a test-harness correction, not a declaration that mounted writable Format 0.19 exists. Disk Format 0.18 remains production.

## 0.18.129 — 2026-10-18

- Fail closed on initial Format 0.19 read-write mount requests using the requested VFS mount flags as well as superblock mode; keep the existing RO-to-RW promotion guard.
- Independently prevent the native writer from enabling Format 0.19 even if a VFS state transition fails to reject a request. Writable Format 0.19 remains disabled until complete crash-safe transaction/resize qualification.
- Strengthen disposable native loopback qualification to request a fresh RW mount before the initial RO mount, avoiding reuse of a previously read-only superblock.

## 0.18.128 — 2026-10-18

- Add the extracted native checkpoint-discovery object to the canonical self-contained DKMS packaging manifest so Linux .deb and .run installations compile exactly the same source as the in-repository kernel build.
- Requalify the bounded native Format 0.19 experimental read-only recovery and existing Format 0.18 mounted behaviour. No on-disk change.

## 0.18.127 — 2026-10-18

- Extract native Format 0.18/0.19 checkpoint validation and bounded recovery-locator discovery into a separately compiled `infiltratorfs_checkpoint_discovery.o` kernel subsystem, without changing on-disk bytes or the read-only 0.19 experimental scope.
- Reduce native `infiltratorfs_core.c` from 117,873 to 108,058 bytes, below the 115,000-byte source-debt ceiling; move the shared candidate contract to the internal header and update the exact native build manifest and source-ownership tests.
- Keep Format 0.19 native writes, resize and physical formatting disabled until full crash-safety validation.

## 0.18.126 — 2026-10-18

- Restore native kernel source maintainability limit for the experimental Format 0.19 read-only selector by simplifying redundant implementation comments. No persistent format or logic change.
- Requalify native Format 0.19 disposable image mounts and existing Format 0.18 mounted regressions under the immutable new source revision.

## 0.18.125 — 2026-10-18

- Add guarded native Linux read-only Format 0.19 recovery from both fixed discovery locators, validating up to six independent checkpoint header candidates and the selected root/index/allocation graph without geometry fallback.
- Refuse native Format 0.19 initial read-write mounts and read-only-to-read-write remounts until writable checkpoint and resize crash protocols qualify; retain normal Format 0.18 behaviour.
- Add disposable 16 MiB in-memory Format 0.19 image emitter and loop-backed native read-only qualification for normal mount, damaged block zero and damaged individual locator. No physical block devices formatted.
- Disk Format 0.19 is **not production-ready**. Writable native mounting, resize, fsck, real interruption and physical-media qualification remain incomplete.

## 0.18.124 — 2026-10-18

- Fix portable Format 0.19 full-graph verification by claiming both fixed discovery-locator blocks as independent, persistently reserved owners, without treating them as ordinary file or checkpoint metadata.
- Reject any allocation-tree page placed on a locator block, and fail closed when either permanent locator block is missing from the committed allocation bitmap.
- Restore the experimental Format 0.19 image conformance test before advancing native mounting or physical formatting. Shipping on-disk Format remains 0.18.

## 0.18.123 — 2026-10-18

- Add an explicit read-only Format 0.19 portable image-volume open API, reusing full committed-graph validation across up to six candidates discovered from independently valid fixed locators.
- Reject 0.19 physical block-device opens and protect caller storage ownership on failures; verify the recovered 0.19 root namespace after block-zero loss, not just its checksum header.
- The current normal filesystem format remains 0.18. Format 0.19 native write/mount, resize and fsck are not yet qualified; do not format physical devices.

## 0.18.122 — 2026-10-18

- Preserve real physical-NVMe performance observations from Format 0.18.110 and 0.18.114 as PERF-019 with benchmark provenance, exact sync-vs-copy timings and qualified interpretation.
- Require bounded microsecond-resolution instrumentation of page-cache writeback, deferred native CoW stages, lock contention and the entire filesystem sync; retain durability barriers and discovery-only locator semantics for Format 0.19.
- Define repeatable physical-hardware performance and post-remount integrity qualifications as independent, currently unmet Format 0.19 release gates. No claims of a performance fix and no on-disk format or runtime changes.

## 0.18.121 — 2026-10-18

- Add an explicit read-only Format 0.19 in-memory/regular-image verification function that searches all locator-discovered checkpoint candidates and reuses the portable core's full allocation, index, namespace, integrity and checksum graph validation.
- Require complete graph validity before accepting a checkpoint generation, and fail closed on operational I/O, resource and incompatible-feature errors instead of treating them as corruption.
- Add test coverage for full-graph selection after primary checkpoint loss, after damage to one locator, and rejection when both locators are unreadable/corrupt.
- Normal formatter/mount, native Linux mounted recovery, resize and fsck remain on Format 0.18; do not enable physical Format 0.19 volumes yet.

## 0.18.120 — 2026-10-18

- Add a guarded in-memory Format 0.19 test-image formatter, sharing the core fresh-format metadata builder while choosing canonical locator-aware initial metadata positions.
- Create and durably flush both fixed discovery locators, account for them in the initial allocation bitmap, and publish three boot checkpoints individually.
- Add image-level assertions for reserved locator blocks, physical-device refusal, exact 0.19 geometry and continued rejection by the active 0.18 reader.
- Add explicit Format 0.19 checkpoint-header validation and bounded, locator-assisted backup header discovery tests with damaged block zero and one or two damaged locators. This does NOT validate complete allocation/object graphs or enable 0.19 mounting. Normal mkfs and kernel mounting still use Format 0.18.

## 0.18.119 — 2026-10-18

- Complete DKMS packaging registration for the preparatory native Format 0.19 locator codec source and header; update the explicit native object policy manifest.
- Restore exact-source release-policy and native package qualification. Active on-disk Format remains 0.18.

## 0.18.118 — 2026-10-18

- Introduce an isolated native Linux kernel Format 0.19 recovery-locator encoder/decoder matching the portable 80-byte CRC64 record contract, with strict geometry, UUID, schema, zero-padding and checkpoint-placement checks.
- Link the new codec into the native DKMS module but leave active mounting/formatting on on-disk Format 0.18 until complete platform recovery and resize qualification.
- Add native/portable byte-contract source parity policy test. No physical drive format changes.

## 0.18.117 — 2026-10-18

- Add deterministic model qualification for Format 0.19 crash/resize publication. Test every durable write boundary with an optional torn page and unavailable block zero, for unchanged, partially relocated and fully relocated secondary checkpoints.
- Include a negative control demonstrating why both backup checkpoints must not be overwritten concurrently.
- The model does not replace native mounted fault injection. Active disk format stays at 0.18 pending full 0.19 integration.

## 0.18.116 — 2026-10-18

- Extend preparatory Format 0.19 locator APIs with bounded discovery from two independent maps, explicitly preserving old and new checkpoint candidates across interrupted resize.
- Add initial metadata placement planning for fixed locator blocks 1 and 2047, preventing initial contiguous allocation metadata from overwriting the second recovery anchor on large media.
- Require individually durable secondary checkpoint refresh in the proposed Format 0.19 resize protocol and strengthen geometry/corruption tests.
- The active on-disk format remains 0.18 pending coordinated native/portable formatter, recovery, resize and fsck integration and fault qualification.

## 0.18.115 — 2026-10-18

- Introduce the independently testable Format 0.19 locator binary codec and fixed-position geometry validator, with CRC64, canonical padding, explicit header/version checks and fail-closed invalid pointers.
- Add conformance tests for corrupted/checksummed-but-malformed records, invalid geometry, old-format identity, duplicate pointers, and stale-but-well-formed candidate records.
- These are preparatory APIs only: Format 0.19 media production, native mount, fsck, and resize remain disabled until qualification is complete. Currently shipped disk Format stays at 0.18.

## 0.18.114 — 2026-10-18

- Accept and document the engineering design for a future Format 0.19 with redundant, fixed-position checkpoint-location records; retain three separately validated authoritative checkpoints.
- Define the compatibility boundary, fault-injection prerequisites and native/portable recovery/resize release gates; leave implementation explicitly unqualified and incomplete.
- No disk format, on-disk representation, driver semantics or runtime behaviour changed. Current disk Format remains 0.18.

## 0.18.113 — 2026-10-18

- Correct the checkpoint-order regression guard to locate the C function boundary without interpreting a literal backslash-n as a line break. No filesystem code or disk format change.

## 0.18.112 — 2026-10-18

- Make native Linux checkpoint publication durably commit one new checkpoint before overwriting secondary recovery copies; preserve transaction-success semantics when later replica flush fails.
- Fail closed when a requested per-object mirror copy count exceeds the configured member count, with no partial write or read.
- Strengthen native durability policy checks and document the unresolved backup-locator recovery case after online resize. On-disk Format remains 0.18 and Libraries stays pinned.

## 0.18.111 — 2026-10-18

- Link platform-adapter system-volume requirements to Filesystems' shared Boot/Setup design and phased acceptance tests.
- Distinguish a working EFI kernel-image reader from a qualified operating-system root adapter.
- Advance immutable source version and Windows INF identity for a documentation-only release; no change to on-disk Format 0.18 or filesystem runtime behaviour.

## 0.18.110 — 2026-10-18

- Consolidate eight duplicated portable format-feature checks behind one shared helper, preserving the original feature predicates and Format 0.18 semantics.
- Centralize three native Linux allocation-locality hint fallbacks without changing placement choices, transaction handling or on-disk structures.
- Verify the sequential-read payload against its source SHA-256 in the maintained InfiltratorFS speed-test utility.
- Advance the semantic release identity to satisfy the immutable-release policy. On-disk Format remains 0.18; Libraries remains pinned to 1.19.46.

## 0.18.109 — 2026-10-17

- Make native Linux `SEEK_DATA`/`SEEK_HOLE` account for dirty buffered page-cache data before writeback, preserving delayed allocation while presenting a coherent sparse-file view.
- Add regression coverage that prevents future sparse-seek fixes from forcing writeback or `fsync`; on-disk Format remains 0.18.

## 0.18.108 — 2026-10-17

- Remove obsolete desktop-package migration metadata; the alpha package owns its bundled integration directly.
- Qualify current package identities without retired component aliases.

## 0.18.107 — 2026-10-16

- Pin Libraries 1.19.46 and use the canonical System Settings provider identity.
- Normalize current product names in source, dependency links and documentation. On-disk Format remains 0.18.

## 0.18.106 — 2026-10-16

- Move the governing engineering philosophy into `docs/PHILOSOPHY.md` and update the documentation index.

## 0.18.105 — 2026-10-16

- Stage and verify release drafts before publication; recover safely from an ambiguous GitHub creation response. Unpublished 0.18.104 is superseded by this candidate.

Keep desktop/presentation regression checks active in optimized builds. A failed contract now reports its expression and exits unsuccessfully even when `NDEBUG` is defined. Production behaviour is unchanged.

This file records user-visible, compatibility, architecture and validation changes for InfiltratorFS.

## 0.18.103 — 2026-10-14
- Restore the public storage-ownership contract on failed portable-volume opens: storage remains caller-owned unless open succeeds, and failed volume state retains no backend alias. Add behavioral regression coverage at both early capability rejection and structural-validation failure.
- Require the project-wide `Build and conformance` workflow as an exact-source release-publication prerequisite, preventing release publication from outrunning sanitizer, static-analysis, portable-core, packaging or Windows build evidence.
- Make the native Windows service random-byte callback fill the complete `size_t` request by chunking BCrypt calls at `ULONG_MAX`, matching the established Win32 storage-adapter contract instead of silently narrowing oversized requests.
- Clarify that native Linux mounted qualification requires a usable running-kernel build environment and fails closed when that evidence cannot be produced.
- On-disk Format remains 0.18; Libraries remains pinned to 1.19.35.

## 0.18.102 — 2026-10-14
- Pin retained LZ4 codec source to immutable upstream commit `ebb370ca83af193212df4dcbadcc5d87bc0de2f0` and use the non-shallow fetch required for exact arbitrary-commit checkout.
- Pin every remote GitHub Action used by maintained workflows to an immutable commit SHA while retaining the human-readable source ref in a comment, so re-running one InfiltratorFS SHA cannot silently execute different automation code.
- Make native Linux mounted qualification fail closed when headers for the running kernel are unavailable; compile-only evidence can no longer masquerade as the mounted qualification required by policy.
- Add an executable dependency-identity policy guard covering mutable workflow actions, the LZ4 source identity and the mounted-native fail-closed boundary. Format 0.18 and the Libraries 1.19.35 pin remain unchanged.

## 0.18.101 — 2026-10-14
- Complete a second philosophy-governed forensic review across architecture, dependency ownership, tests, packaging, documentation and release machinery without changing Format 0.18 semantics.
- Remove hidden GitHub environment coupling from semantic release resolution; release-candidate packaging now binds its version explicitly from the resolver output, and release policy guards that boundary.
- Restore one documentation authority map by classifying the maintained portable-security and filesystem-paper audit documents, and make the workflow definitions themselves the executable authority for the automatic validation gate set.
- Correct the maintained IAC1 hardware-design note so it no longer contradicts Format 0.18's retained non-default LZ4 codec representation.
- Preserve the exact Libraries 1.19.35 pin after rechecking newer Common releases against the filesystem-facing contracts actually consumed here.
- On-disk Format remains 0.18.

## 0.18.100 — 2026-10-13
- Correct the normative fsck/scrub contract so it reflects the already-qualified deterministic checkpoint-replica repair path instead of claiming that repair is unimplemented.
- Strengthen the fsck structural policy guard so documentation cannot regress away from the deliberately narrow repair boundary: automatic repair requires a surviving valid checkpoint replica and an otherwise valid structural graph; ambiguous corruption remains fail-closed.
- Advance the semantic source identity after the published 0.18.99 release without changing Format 0.18 or the exact Libraries 1.19.35 pin.

## 0.18.99 — 2026-10-13
- Repair the Linux package configuration race so delayed UDisks formatter advertisement cannot make an otherwise valid package installation fail; explicit integration verification remains strict.
- Advance the semantic source identity after the published 0.18.98 release so post-release work cannot reuse an immutable release version.
- Correct current-status documentation for the native Windows driver: implementation exists, while real mounted Windows and Driver Verifier qualification remain the explicit release gate.
- Consolidate shell-side version consumers on the project version authority and remove stale documentation claims discovered by the philosophy-governed forensic review.
- On-disk Format remains 0.18; the exact Libraries 1.19.35 pin is unchanged.

## 0.18.98 — 2026-10-13
- Complete a philosophy-governed forensic architecture, test, packaging and release review without changing Format 0.18 semantics.
- Centralize DKMS source staging so packaging, hosted kernel reproduction and destructive qualification consume one authoritative component manifest.
- Strengthen release identity so source cannot reuse an already-published semantic version at a different commit, and reject version rollback relative to reachable releases.
- Remove the retired `.github/release-trigger` path from native CI and correct the IAC1 hardware document to the implemented physical-core N-1 policy.
- Clarify that Windows source-shape guards are not a substitute for mounted Windows and Driver Verifier qualification.
- Make the native maintainability guard consume the canonical DKMS manifest instead of asserting the removed duplicated source lists, and make GitHub checks—not a stale hand-maintained table—the live authority for automatic exact-SHA qualification evidence.
- On-disk Format remains 0.18.

## 0.18.97 — 2026-10-07
- Build final Linux release artifacts once from pinned inputs and publish the exact qualified bytes instead of rebuilding during publication.
- Remove duplicated release-qualification coupling and verify exact artifact lineage through checksums and byte-for-byte post-publication comparison.
- On-disk Format remains 0.18.

## 0.18.96 — 2026-10-07
- Simplify the release qualification pipeline while retaining exact-source native kernel, root-volume, Linux metadata and package qualification prerequisites.
- Decouple root-volume semantic qualification from the separately pinned desktop integration bundle.
- On-disk Format remains 0.18.

## 0.18.95 — 2026-10-07
- Restore the single-package Linux desktop integration model and publish corrected integrated Linux packages.
- Decouple root-volume filesystem semantics from desktop bundle mechanics and prove installation of the released package.
- Remove a duplicated storage-policy fallback check without weakening persistent storage policy semantics.
- On-disk Format remains 0.18.

## 0.18.94 — 2026-09-26
- Add persistent per-object protection-copy and encryption-domain policy for file data while keeping structural metadata and checkpoints on the backend default protection policy.
- Route ordinary, compressed, truncate, hole-punch, scrub and named-stream data through policy-aware storage I/O; nondefault-policy files remain extent-backed so inline metadata cannot bypass placement or encryption.
- Add deterministic object-identity-based mirror placement and per-domain AES-256-GCM key separation authenticated by domain ID.
- Reject unsafe metadata-only storage-policy changes on nonempty files; presentation flags remain independently mutable.
- Complete Windows basic-attribute, timestamp, owner/group/DACL and lossless filename projection through the ProjFS bridge.
- Record selected-object restore from retained snapshots as implemented.
- Keep macOS, BSD and Haiku out of the delivery roadmap; they are not current implementation targets.
- On-disk Format remains 0.18.

## 0.18.93 — 2026-09-25
- Align the Linux Manager minimum window geometry with the suite desktop shell while preserving the 1220×780 default size.
- Keep filesystem format, kernel driver, storage, encryption, namespace, maintenance behaviour, dependencies and Common APIs unchanged.
- On-disk Format remains 0.18.

## 0.18.92 — 2026-09-25
- Repair the managed libblockdev patch so its InfiltratorFS technology switch uses the actual extended enum member instead of a preprocessor fallback that fails strict `-Werror=switch` builds.
- Give Linux Manager stack-switcher tabs the suite-wide 30 px desktop control height while retaining the existing compact tab rhythm.
- Keep filesystem format, kernel driver, storage, encryption, namespace and maintenance behaviour unchanged.
- On-disk Format remains 0.18.

## 0.18.91 — 2026-09-25
- Give the Linux Manager titlebar the suite-wide 6 px side padding used by the publisher shell.
- Keep filesystem format, kernel driver, storage, encryption, namespace, maintenance behaviour unchanged.
- On-disk Format remains 0.18.


## 0.18.90 — 2026-09-25
- Align Linux Manager stack-switcher tabs with the suite-wide 5 px / 10 px compact tab rhythm.
- Keep filesystem format, kernel driver, storage, encryption, namespace, maintenance behaviour unchanged.
- On-disk Format remains 0.18.

## 0.18.89 — 2026-09-25
- Align Linux Manager action rows with Common's 10 px control radius instead of the 6 px compact radius.
- Keep filesystem format, kernel driver, storage, encryption, namespace, maintenance behaviour unchanged.
- On-disk Format remains 0.18.

## 0.18.88 — 2026-09-25
- Align ordinary Linux Manager cards with Common's 12 px card radius instead of the 10 px control radius.
- Make hosted N-1 CPU qualification explicitly skip the runtime multi-worker proof when the runner exposes only one filesystem worker slot; the physical-core budget check still runs, and multi-worker proof remains mandatory on capable runners.
- Keep filesystem format, kernel driver, storage, encryption, namespace, maintenance behaviour unchanged.
- On-disk Format remains 0.18.

## 0.18.87 — 2026-09-25
- Correct the native N-1 CPU policy to reserve one complete physical core, not merely one logical SMT thread, for the operating system and interactive applications.
- Derive the module-wide CPU-heavy worker budget from unique online physical-core sibling groups. A 12-core/14-thread hybrid system therefore uses 11 filesystem workers instead of 13.
- Keep SMT siblings available to the Linux scheduler as general capacity without letting them inflate InfiltratorFS's CPU-heavy concurrency ceiling.
- Align mounted CPU qualification, scale-stress defaults, architecture documentation and shipped synchronization contracts with the physical-core N-1 rule.
- On-disk Format remains 0.18.

## 0.18.86 — 2026-09-24
- Cache resolved effective project identity on ordinary inodes across per-folio quota reservations, with epoch invalidation on project-root changes and cross-parent namespace moves; multiply-linked/no-parent files remain on the exact alias-resolution path.
- Repair deferred-transaction private-block detection. The working transaction and live allocation bitmap intentionally share one image, so the former bitmap-difference test could never identify unpublished blocks; an exact volatile allocation set now allows safe in-place reuse with the existing undo journal instead of repeated metadata CoW churn.
- Make Linux xattr/special-metadata lookup lazy and scalable. A cache miss now performs one exact directory-tree lookup for the deterministic UUID sidecar instead of scanning every sidecar and performing an object-index lookup for each.
- Materialize Format 0.18 fallocate reservations in the native 4 MiB writer unit instead of 64 KiB fragments while preserving uncompressed initialized-allocation semantics.
- Submit all three checkpoint replicas together under one block plug after the dependency durability barrier, retaining the final device-cache flush and fail-closed replica handling.
- Avoid rebuilding the complete free-extent accelerator on tiny fsync publications; bounded small retire sets are merged incrementally while large publications retain the bitmap rebuild path.
- Parallelize crash-orphan discovery across the filesystem-wide N-1 CPU budget in a dedicated compiled component; authoritative orphan reclamation remains serialized and revalidated.
- Add regression guards for transaction-private CoW reuse, lazy xattr lookup, batched fallocate/checkpoint publication, small-publication free-index maintenance and N-1 orphan discovery.
- On-disk Format remains 0.18.

## 0.18.85 — 2026-09-24
- Align the Linux Manager sidebar divider with the suite-wide 248 px desktop navigation width.
- Keep filesystem format, kernel driver, storage, encryption, namespace, maintenance behaviour unchanged.
- On-disk Format remains 0.18.

## 0.18.84 — 2026-09-24
- Remove the whole-volume object-index snapshot from ordinary quota admission. Single-parent files/directories now resolve project ownership through their bounded parent chain, while the expensive alias scan is retained only for multiply-linked/no-parent files that genuinely require it.
- Skip project ownership resolution entirely for user/group-only quota workloads when no project quota rule is active.
- Add a static scalability guard so normal quota admission cannot silently regress to whole-index project discovery.
- Keep the filesystem-wide N-1 CPU budget unchanged: cheap admission remains synchronous by design, while genuinely independent heavy filesystem work continues to use the shared N-1 execution budget.
- On-disk Format remains 0.18.

## 0.18.83 — 2026-09-24
- Align Manager secondary device metadata with the suite-wide 11 px supporting-text scale.
- Keep filesystem format, kernel driver, storage, encryption, namespace, maintenance behaviour unchanged.
- On-disk Format remains 0.18.

## 0.18.82 — 2026-09-24
- Align Manager device navigation side margins with the suite-wide 8 px navigation rhythm.
- Keep filesystem format, kernel driver, storage, encryption, namespace, maintenance behaviour unchanged.
- On-disk Format remains 0.18.

## 0.18.81 — 2026-09-24
- Align the Manager empty-state heading with the same 28 px publisher title scale as populated Manager pages.
- Keep filesystem format, kernel driver, storage, encryption, namespace, maintenance behaviour unchanged.
- On-disk Format remains 0.18.

## 0.18.80 — 2026-09-24
- Align the Manager hero heading with the 28 px publisher-wide desktop heading scale.
- Keep filesystem format, kernel driver, storage, encryption, namespace, maintenance behaviour unchanged.
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
- Preserve the exact Libraries 1.19.24 pin at `748e089ae175329471d4cf375522c44081371bd5`.

## 0.18.75 — 2026-09-22

- Fix the residual sustained-write publication stall isolated by the 0.18.74 live phase telemetry: transaction publication was spending almost all tail latency inside allocation-map publication while dependency draining had fallen to only tens of milliseconds.
- Replace per-range incremental free-extent cache insertion during transaction publication with one authoritative word-at-a-time rebuild from the post-publication allocation bitmap. This removes the fragmented-volume O(n²) scan/memmove path while preserving the exact bitmap as allocation authority.
- Add allocation-map subphase telemetry for preparation, retired-range journalling, bitmap/index reclamation, leaf writes and branch writes, including dirty-tree counts, so any remaining publication tail can be attributed without another broad kernel trace.
- Keep operation-level rollback on incremental extent-cache repair because those rollback sets are bounded; only large transaction publication uses the one-shot rebuild.
- On-disk Format remains 0.18.

## 0.18.74 — 2026-09-22

- Publish 0.18.74 as the sustained native-write stall recovery release after the 0.18.73 development cycle; on-disk Format remains 0.18.
- Advance the exact Libraries dependency from 1.19.23 to 1.19.24 at `748e089ae175329471d4cf375522c44081371bd5`. This Common release hardens graphics surface range/alias handling; InfiltratorFS does not consume that graphics API, so the pin changes no filesystem or on-disk semantics.
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
- Advance the exact Libraries dependency from 1.19.20 to 1.19.23 at `a9cf2957cffeefe6001830916b8a32c2ef58a551`; the intervening Common releases are backward-compatible hardening plus a temporal helper and do not alter InfiltratorFS persistent semantics.
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
- Gate release assembly and publication on the presence of that embedded desktop-integration bundle and on the core Debian package carrying its required desktop integration.

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

- Advance to Libraries 1.19.10 and consume the complete Linux MBLink-derived appearance roles in the native Manager, including titlebar, connection/status surface, heading/summary/kicker/detail/note text, selected-summary, accent-hover/foreground and muted state-border colours.
- Keep filesystem-specific semantic colouring on top of those canonical roles: success for mounted/check, warning for offline/scrub, fault for destructive formatting, information for inspection and the canonical blue accent for filesystem/forensic operations, without changing layout or behaviour.

## 0.18.63 — 2026-09-19

- Complete native Linux mutation of scalable extent-pointer trees, including promotion beyond the direct page-head ceiling and safe demotion when extent metadata shrinks.
- Avoid a second whole-device writeback walk during transaction publication: stage dependencies durably, synchronously submit the three checkpoint buffers, then issue the device-cache flush.
- Avoid whole-device `sync_blockdev()` work for clean `fsync()` calls when no native transaction exists to publish.
- Add slow-path diagnostics for checkpoint selection, writable mount initialisation, checkpoint healing, quota reconstruction, snapshot retention-map construction and transaction publication.
- Report checkpoint-replica divergence when it forces expensive deep graph validation so recovery-time stalls are directly attributable in the kernel log.
- Preserve crash-safe checkpoint ordering and fail-closed write poisoning on indeterminate durability failures.
- Advance userspace to Libraries 1.19.8 and consume its canonical typography, design metrics, font provenance, project identity, formatting and POSIX path contracts where they fit without moving filesystem semantics into Common.
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
- Advance the shared userspace foundation to Libraries 1.19.3.
- Keep the on-disk format at 0.18.

## 0.18.59 — 2026-09-18

- Consolidate product-neutral userspace parsing, exact I/O and checked dynamic-array growth on Libraries 1.19.2.
- Keep filesystem semantics and the on-disk Format 0.18 contract unchanged.

## Historical source

Git tags and GitHub Releases remain authoritative for exact historical source and release assets. Existing specialist release notes remain valid where present.

## Policy

Record behaviour changes, fixes, compatibility changes and support-boundary changes. Do not invent historical detail that cannot be tied to a release or commit.
