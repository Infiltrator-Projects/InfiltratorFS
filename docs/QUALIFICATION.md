<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Qualification Ledger

The forensic cross-check of current kernel invariants against the 2026
filesystem research/advisory failure modes is recorded in
[`FILESYSTEM-PAPER-AUDIT-2026.md`](FILESYSTEM-PAPER-AUDIT-2026.md).

This file is the **authoritative qualification contract and curated milestone/manual evidence ledger** for InfiltratorFS. Live automatic exact-source evidence is owned by the immutable GitHub Actions checks attached to the exact commit and, for a published version, by its immutable tag and GitHub Release. This file is not a feature list; feature completion belongs in `ROADMAP.md`.

Historical results recorded here apply only to the source commit on which they ran. They are deliberately not a hand-maintained mirror of every automatic workflow run. A later green portable CI run does not silently inherit an older mounted, heavy or destructive qualification result.

## Qualification classes

- **Build and conformance** — ordinary broad CI: GCC/Linux, Clang, sanitizers, static analysis, Windows portability/interoperability, policy guards and package construction. Every automatic job is capped at 10 minutes.
- **Native Linux kernel module** — builds the module/DKMS source on every main revision and requires the runner's exact running-kernel headers. Its automatic release gate includes Format 0.19 native write/recovery, real live remounts, removable/casefold names, quotas, CPU parallelism and a bounded mounted short-write/1023-byte filename/scrub test. Unavailable mount prerequisites fail closed, not compile-only. The exhaustive 512 MiB transaction/scrub workload and separate mounted grow/shrink, media-placement and defragmentation suites run on explicit manual dispatch; they are not represented as automatic release evidence.
- **Native resize qualification** — dedicated mounted grow/shrink qualification independent of the quota gate; automatic execution is capped at 10 minutes.
- **Heavy filesystem qualification** — million-file/1 TiB scale plus near-full mixed-workload endurance. This is manual-only milestone evidence because it intentionally exceeds the automatic CI ceiling.

The mounted scale harness compiles `tests/native-scale-stress.c` and uses it for
the million-file create, churn, and post-remount verification phases. CI runs
small cross-verification workloads against the retained Python implementation
to catch changes to file naming, sentinel contents, or population checks.
The live remount harness compiles `tests/native-remount-qualification.c` to
exercise `mount(2)` directly; its Python predecessor remains as a reference
until both implementations can be compared on a disposable native mount.
The mounted concurrency gate compiles `tests/native-concurrency-qualification.c`
and runs its separate-process mutators four times. The ACL remount gate
compiles `tests/native-posix-acl-qualification.c`, which encodes the Linux ACL
xattr format directly and still checks `setfacl`, `getfacl`, and `rsync -aA`.
The near-full qualification compiles `tests/native-endurance-stress.c`; its
SHAKE-256 data and JSON integrity manifest are cross-verified in both
directions against the retained Python reference during ordinary CI.
- **Real root-boot qualification** — real UEFI boot, live-root package upgrade, forced power-loss recovery and repeated scrub. This is manual-only and is run when root/boot/recovery risk warrants it.
- **Formatter integration qualification** — pinned libblockdev/UDisks/GNOME Disks integration build and end-to-end formatter/probe checks. The full upstream-stack build is manual-only because it intentionally exceeds the automatic CI ceiling.
- **Physical partition qualification** — explicitly destructive operator-run qualification on dedicated media; never unattended ordinary CI.
- **Release publication gate** — requires successful same-source automatic release prerequisites, then installs the generated package, verifies native filesystem registration, mounts a real Format 0.19 image, performs non-zero write/read verification, syncs, unmounts and requires a CLEAN scrub. It also rejects restoration of the legacy FUSE product path. Long manual qualifications are supporting evidence, not mandatory automatic release blockers.

A workflow should fail closed when a qualification class claims mounted coverage but the required running-kernel environment is unavailable. Merely skipping mounted work must not be treated as equivalent evidence.

## CI duration policy

Automatic qualification and conformance jobs are hard-capped at 10 minutes where they are part of the ordinary CI set. A test that intrinsically needs more than 10 minutes must be a manual-only qualification and is launched deliberately when its coverage is warranted. The native kernel workflow therefore runs a separate bounded, real-mounted smoke gate automatically and reserves the 512 MiB extended read/write/scrub and online maintenance tests for manual dispatch. Native resize also has an independent automatic qualification workflow. The other long-running manual suites include real UEFI/root crash recovery, full GNOME/libblockdev/UDisks formatter build, and million-file/1 TiB endurance. A watchdog cancellation never counts as passing evidence.

## Curated development milestone evidence

The entries below preserve useful milestone evidence. For the current source revision, automatic qualification state must be read from the checks attached to that exact Git commit; absence of a newer hand-entered row here does not imply absence of automatic qualification.

### 0.19.1 interrupted-resize recovery correction

The 18 October 2026 forensic comparison reproduced a gap in 0.19.0: damage
locator B after writable open, durably publish A's new resize map, then tear
the first primary-checkpoint write. Two valid old backups survived, but their
positions were no longer discoverable. Restoring only the old B hint recovered
the complete data and a CLEAN scrub. The extended real-writer test failed
against the released primary-first protocol before the correction.

The corrected shared publisher enforces A, secondary 1, secondary 2, primary,
B, with each record independently flushed. The old primary's geometry remains
available until the new backups are durable. No disk layout or Format 0.19
version change is required.

Local GCC 13.3 Debug qualification passed all 57 CTest cases and all 24 policy
guards referenced by the workflows. The actual-writer matrix passed 296
ordinary-publication recovery/scrub cases, 256 grow/shrink recovery/repair
cases, and 28 interruptions during writable checkpoint/locator healing (580
total). Latent locator damage is injected after writable admission; cold
recovery uses the normal volume API, storage enforces physical bounds, and
writable reopen must repair all redundancy before resumed mutation.

The updated module also passed the disposable QEMU Linux
`6.8.0-138-generic` mounted Format 0.19 suite: portable/native exchange,
write/remount, repeated grow/shrink relocation, actual backing-file reduction,
stale hints, bootstrap damage, read-only non-mutation, writable repair,
feature admission and old-format rejection. Windows prefix offsets were
cross-checked against the canonical packed superblock and a freshly formatted
image; WDK compile-time assertions now preserve those offsets.

Exact-source automatic release gates remain the Actions authority. Physical
power-loss/endurance, PERF-019 measurements and mounted native-Windows
qualification retain the separate scope recorded below.


### 0.19.0 recovery implementation and local mounted qualification

The 18 October 2026 testing implementation replaces the former image-only/read-only scope with normal Format 0.19 formatting, portable and native writes, staged grow/shrink and writable checkpoint/locator repair. Portable and Linux writers share the same publication ordering header. Live exact-source release status remains the Actions authority described above.

Local GCC 13.3 Debug conformance includes the real-storage fault harness: 148 ordinary publication interruption/recovery cases and 128 grow/shrink interruption/recovery cases, with torn writes, ambiguous flush completion, block-zero loss, cold graph/data verification, writable healing and resumed writes. Default CLI tests reject unaligned images without modifying them and repair locator-only degradation. The older standalone crash model is retained independently.

An isolated QEMU VM booted Ubuntu Linux `6.8.0-138-generic` with the module built from this working source. Native qualification passed normal formatting, portable/native data exchange, initial writable mount, RO-to-RW remount, namespace writes, grow/shrink/repeated relocation, stale fixed hints with a newer primary, physical reduction of the backing file, and deep post-remount scrub. Damage cases `[0]`, `[1]`, `[2047]`, `[1,2047]`, `[0,1]` and `[0,2047]` each passed non-mutating read-only recovery followed by writable repair and new writes. Loss of all three bootstrap positions was refused without modifying the image.

These results establish readiness for testing. They do not claim physical NVMe performance, real hardware power-loss durability, root-boot/endurance requalification or mounted native-Windows qualification; those classes remain separately tracked.

### 0.18.78 deferred-publication failure hardening baseline

Exact implementation source `4e15491c143bcd39d0c07daa665190e68242840d`
completed the filesystem-critical automatic qualification on 2026-09-24:

- **Native Linux kernel module** run `35948791030` passed, including the
  terminal deferred-publication durability guard, deferred namespace
  publication guard, generic and running-kernel module builds, DKMS source-root
  reproduction, mounted read/write/remount, removable-name and casefold policy,
  N-1 CPU parallelism, user/group/project quotas, native transaction plus CLEAN
  scrub, online grow/shrink, media-aware placement and online defragmentation.
  Its independent upstream Linux 7.0 compile-compatibility job also passed.
- **Linux metadata qualification** run `35948791047` passed the quota, ACL and
  high-scale xattr durability workload.
- **Linux root-volume qualification** run `35948791097` passed package,
  initramfs/root payload and mounted root metadata/API checks.
- **Native resize qualification** run `35948791059` passed.
- Within **Build and conformance** run `35948791042`, the Linux full suite,
  release-policy gate and Windows native/portable-core job passed on this exact
  implementation source. Remaining independent compiler/sanitizer/package jobs
  are ordinary broad CI rather than evidence substituted for the native mounted
  durability qualification above.

This baseline closes the deferred-publication failure-state defect found in the
0.18.78 forensic review. A failed publication is terminal for the current
mount: the same possibly-indeterminate transaction is not retried by later
fsync, idle work or unmount, and threshold-triggered inline writeback propagates
the publication failure into Linux writeback error handling. The obsolete
flush-before-create/mkdir/setattr data wrappers and their macro alias bridge are
also retired; the active POSIX/native namespace path remains deferred. On-disk
Format remains 0.18.

### 0.18.77 published small-file writeback baseline

Exact released source `0de3f22537c3217fff7f029a4f66de6b1fcd7706`
completed the automatic release qualification on 2026-09-24:

- **Build and conformance** run `35941198409` passed.
- **Native Linux kernel module** run `35941198457` passed.
- **Linux metadata qualification** run `35941198394` passed.
- **Linux root-volume qualification** run `35941198435` passed.
- **Automatic CI ten-minute watchdog** run `35941204741` passed.
- **Release artifacts** run `35941507655` passed and the immutable-version
  publication sequence produced GitHub release `v0.18.77`.

That release is the published baseline for the small-file inline writeback
lock-scope/revalidation work. Development source 0.18.78 intentionally advances
beyond it while retaining on-disk Format 0.18.

### 0.18.72 Common 1.19.23 integration baseline

Exact integration source `b77327f222b499f0834fd41bd03c27f8f644544a` advanced the pinned/submodule Common dependency to
1.19.23 at `a9cf2957cffeefe6001830916b8a32c2ef58a551` and completed the automatic
qualification set on 2026-09-22:

- **Build and conformance** run `35703040492` passed.
- **Native Linux kernel module** run `35703040479` passed.
- **Linux root-volume qualification** run `35703040567` passed.
- **Linux metadata qualification** run `35703040519` passed.
- **Native resize qualification** run `35703040501` passed.
- **Windows Explorer bridge qualification** run `35703040471` passed.

Common 1.19.21 through 1.19.23 are compatible hardening releases for this
consumer. They add no filesystem-format semantics and require no InfiltratorFS
compatibility shim. The filesystem continues to consume Common only for neutral
shared primitives/tooling contracts, not for persistent filesystem meaning.


### 0.18.72 case-folded namespace and typed-extension completion baseline

Exact implementation/qualification source `397d2c0f7ba38cb21ea5cf81729f0b952912e22f` completed the feature-specific qualification on 2026-09-22:

- **Build and conformance** run `35701731801` passed GCC static analysis, Clang conformance, ASan/UBSan, the Linux full suite, native package construction and the Windows native/portable-core build. The Linux full suite and Windows tests both built and passed `infilfs-casefold` and `infilfs-typed-extensions`.
- **Native Linux kernel module** run `35701731792` passed the dedicated **Native case-folded namespace policy** step on a real mounted `mkfs.infilfs --casefold` image. The test verified mixed-case lookup identity, VFS/dcache inode identity, `O_EXCL` collision rejection, original-spelling preservation, ASCII-only v1 folding, write-through via alternate case, clean unmount and CLEAN scrub. The same run also passed upstream Linux 7.0 compile compatibility for the native reader including typed-extension validation.
- **Linux metadata qualification** run `35701731857` passed on the exact source.

Case-fold policy v1 is therefore complete: it is opt-in, persistent, locale-independent,
preserves original UTF-8 spelling, folds ASCII A-Z only for namespace identity,
and uses the same identity rule in portable lookup/cache/tree/scrub paths and
the native Linux VFS/dcache path.

Generic typed/reparse extension objects are also complete. Format 0.18 now has
immutable indexed `INFS_OBJECT_EXTENSION` envelopes attached through the
existing `extended_attributes_object_id`, with stable 128-bit type IDs,
independent type versions, bounded opaque payloads, SHA-256 payload digests,
fail-closed flags/version validation, portable set/get/detach APIs, scrub
reference validation, native-reader recognition and reflink preservation.
Unknown extension semantics remain opaque to the core. Detach/replacement does
not perform an unsafe foreground last-reference free; shared unreachable
extension objects remain eligible for the same snapshot-aware graph-tracing
maintenance model used by other immutable shared metadata.


### 0.18.71 platform-specific security-preservation policy baseline

Exact policy source `667d722f73dba09047329a15724af1f20fbd2916` completed the required cross-platform policy qualification on 2026-09-22:

- **Native Linux kernel module** run `35683597910` passed, including the mounted read/write transaction and CLEAN scrub matrix plus upstream Linux 7.0 compilation.
- **Linux metadata qualification** run `35683597932`, **Linux root-volume qualification** run `35683597969` and **Native resize qualification** run `35683598316` passed on the same source.
- **Windows Explorer bridge qualification** run `35683597922` passed on the same source.
- Within **Build and conformance** run `35683597943`, the Linux full-suite job `106605679862` and Windows native/portable-core job `106605978513` passed the policy-sensitive portable-security and cross-platform conformance tests on the same source.

The completed preservation contract requires adapters to map common meaning
into portable semantics, retain genuinely platform-specific residual state,
refuse mutations that would silently discard unrepresentable security metadata,
and ensure opaque/unknown metadata cannot manufacture access. This completion is deliberately limited to the preservation *rules*. Generic
typed extension objects are now separately complete under the 0.18.72
qualification baseline above; portable named metadata/streams remain separate
roadmap work.

### 0.18.70 portable security-object completion baseline

Exact implementation source `6212c44f9d55b4ecc3125a127fb10d64f18ee8d3`
completed the required cross-platform qualification on 2026-09-22:

- **Build and conformance** run `35625432756` passed in full. The Linux full suite built and passed `infilfs-security-policy` and `infilfs-security-objects`; ASan/UBSan also passed the security-object test, and the same workflow's Windows native/portable job built and passed both tests under MSVC.
- **Native Linux kernel module** run `35625432736` passed the out-of-tree module, native mounted read/write/remount/scrub qualification and upstream Linux 7.0 compile gate on the same exact source.
- **Linux metadata qualification** run `35625432952` and **Linux root-volume qualification** run `35625432770` passed on the same exact source.

The qualified implementation provides stable 128-bit portable principals,
scoped POSIX and canonical Windows SID bindings, preservation-only opaque
bindings, bounded persistent reverse lookup, immutable shareable descriptors,
ordered allow/deny evaluation, inheritance, paged large ACLs, transactional
attachment/removal, native-reader recognition and scrub validation. The
portable security-object roadmap item is therefore complete. Platform-specific
security metadata that has no portable equivalent remains governed by the
separate preservation-policy and generic-metadata roadmap items.

### 0.18.66 Windows security-descriptor mapping baseline

Exact implementation source `7ce6a641de9de69fa67e32c21d979641082460b8`
completed the required cross-platform qualification on 2026-09-20:

- **Build and conformance** run `35506988395` passed the Linux full suite, Clang conformance, ASan/UBSan, GCC static analysis, native package construction and the Windows native application/portable-core build and tests;
- **Windows Explorer bridge qualification** run `35506988393` passed on the same exact source;
- **Native Linux kernel module** run `35506988424` passed, including the upstream Linux 7.0 compile gate and mounted native qualification; and
- the exact source also passed **Linux metadata qualification** run `35506988373`, **Linux root-volume qualification** run `35506988389` and **Native resize qualification** run `35506988359`.

`include/infilfs/win32_security.h` now provides an executable, host-header-independent
ACCESS_MASK projection onto the portable rights vocabulary. File and directory
specific bits retain their distinct Windows meanings, GENERIC_* masks are
expanded before projection, DELETE/READ_CONTROL/WRITE_DAC/WRITE_OWNER map to
portable administrative rights, and SYNCHRONIZE/ACCESS_SYSTEM_SECURITY/
MAXIMUM_ALLOWED cannot manufacture persistent portable access. SID identity
remains an adapter binding rather than the persistent principal identity; DACL
allow/deny ordering and inheritance remain responsibilities of the future
versioned portable security object.

### 0.18.66 portable-rights and Linux security-mapping baseline

Exact implementation source `cc8f7215398b11a9b637ca5ea02268be67b65de5`
completed the required automatic qualification on 2026-09-20:

- **Build and conformance** run `35506393848` passed in full, including the new portable security-policy conformance test, Linux/Clang builds, sanitizers, GCC static analysis, native package construction and Windows portable/native builds;
- **Linux metadata qualification** run `35506393878` passed the mounted UID/GID/mode, real POSIX ACL, default-ACL inheritance, chmod-mask interaction, rsync ACL preservation and remount durability contract;
- **Linux root-volume qualification** run `35506393873` passed; and
- **Native Linux kernel module** run `35506393838` passed, including upstream Linux 7.0 compilation and the complete mounted native read/write/scrub suite.

The portable rights mask now has stable filesystem-meaning bit assignments in
`include/infilfs/security.h`, independent of Linux and Windows ABI constants.
The Linux projection is executable in `include/infilfs/posix_security.h`:
UID/GID remain adapter-local bindings, regular-file and directory rwx semantics
map separately, POSIX ACL entries use the same projection after Linux effective
masking, and mode bits cannot manufacture permission-administration or
ownership rights. The final portable principal/security-object store remains a
separate roadmap item.


### 0.18.66 native N-1 CPU-budget baseline

Exact implementation source `3adfb38ee91389f757617df892018aa5dc8fdce5`
completed the required qualification on 2026-09-20:

- **Build and conformance** run `35498743024` passed in full, including Linux full suite, GCC/Clang, ASan/UBSan, static analysis, native Linux packages and Windows builds;
- **Native Linux kernel module** run `35498743019` passed in full, including upstream Linux 7.0 compilation and the dedicated mounted **Native N-1 CPU parallelism qualification** step;
- **Linux metadata qualification** run `35498743164` passed;
- **Linux root-volume qualification** run `35498743069` passed; and
- **Native resize qualification** run `35498743106` passed.

The dedicated mounted workload resolved a filesystem CPU budget of 3 on a
4-logical-CPU runner and measured a real prepared-write worker peak of exactly
3, with 512 preparation attempts and 512 successful prepared appends. The
workers execute digest/compression/reservation preparation before the serialized
publication section and each occupies the shared module-wide N-1 gate, so this
is direct evidence that independent write preparation can fill the complete
filesystem budget without exceeding it. The same run subsequently completed the
full mounted read/write, quota, media-placement, defragmentation and scrub suite.


### 0.18.66 removable-volume filename-profile baseline

Exact implementation source `d4e91557ec4f4a5590450294e0f40ec10473b728`
completed the required automatic qualification on 2026-09-20:

- **Build and conformance** run `35498348227` passed, including the portable removable-name conformance coverage, GCC/Clang, sanitizers, static analysis, Linux package construction and Windows builds;
- **Native Linux kernel module** run `35498348121` passed, including the dedicated mounted **Native removable-volume filename profile** step plus the complete native read/write/scrub suite and upstream Linux 7.0 compile gate;
- **Linux metadata qualification** run `35498348272` passed;
- **Linux root-volume qualification** run `35498348190` passed; and
- **Native resize qualification** run `35498348279` passed.

The optional persistent `REMOVABLE_NAMES_V1` incompatibility feature is now
qualified as a complete cross-platform filename profile. It retains exact UTF-8
identity while enforcing the conservative removable-media component contract in
both portable and native mutation paths and fails closed when on-disk directory
metadata violates the selected profile.


### 0.18.66 Unicode-policy and deterministic-repair baseline

Exact implementation source `8f89ea23fac7f52dc6f4f8f9ea46e9d161dce35c`
completed the relevant automatic qualification on 2026-09-20:

- **Build and conformance** run `35495809194` passed, including the Linux full suite, format conformance, phase-3 integrity/repair cases, Clang, ASan/UBSan, GCC static analysis, native Linux package construction and Windows portable/native application builds;
- **Linux metadata qualification** run `35495809198` passed on a freshly formatted current-format native mount with the new Unicode-policy feature bit; and
- **Linux root-volume qualification** run `35495809170` passed on the same exact source.
- **Native Linux kernel module** run `35495809153` passed, including the mounted native read/write/scrub suite and upstream Linux 7.0 module compatibility.

The Unicode policy is persisted as incompatible feature `UNICODE_NORM_V1`.
It deliberately defines exact validated UTF-8 preservation with no implicit
normalization, so future normalization semantics cannot silently alter lookup.

The deterministic repair qualification proves both one- and two-replica
checkpoint loss can be healed from the surviving validated committed graph,
while non-checkpoint authoritative metadata corruption is left byte-for-byte
unchanged and reported uncorrected. These are the bounded unambiguous repair
semantics required by the roadmap; ambiguous corruption remains fail-closed.

### 0.18.65 pre-release implementation baseline

Exact development source `4696539ad0446dd92b7a3527137100caceaac0e6`
passed the broad and native automatic gates on 2026-09-20:

- **Build and conformance** run `35493759241`;
- **Native Linux kernel module** run `35493759240`, including the new exact upstream Linux 7.0 compile-compatibility job.

The immediately preceding implementation source
`4bdc46c6b0fcbf589d1bac3971e9ae808d71d552` also passed:

- **Linux metadata qualification** run `35493659875`;
- **Linux root-volume qualification** run `35493659867`; and
- **Native resize qualification** run `35493659900`.

The only executable-policy change between those two sources is the corrected
self-contained upstream Linux 7.0 compiler gate. The immutable `v0.18.65`
release still requires its own exact release commit to pass the event-driven
publication prerequisites before a tag or release is created.

### 0.18.60 pre-release implementation baseline

Exact implementation source `ee716fc61310ed6c3d9e07852b72883518793d5e`
passed the complete ordinary automatic qualification set on 2026-09-19:

- **Build and conformance** run `35430162303`;
- **Native Linux kernel module** run `35430162314`;
- **Linux metadata qualification** run `35430162323`;
- **Linux root-volume qualification** run `35430162302`; and
- **Native resize qualification** run `35430162431`.

This is the implementation baseline immediately before the 0.18.60 release
metadata/version commit. The immutable release publisher still requires the
exact release commit to pass its same-source automatic gates before creating
`v0.18.60`; these results are not substituted for that release gate.

### 0.18.59 qualified implementation baseline

Exact implementation source `49dfae888cf2077bfb871207e2190030936aa6d3`
passed all ordinary push qualification workflows on 2026-09-18:

- **Build and conformance** run `35335992730`;
- **Native Linux kernel module** run `35335992738`;
- **Linux metadata qualification** run `35335992618`;
- **Linux root-volume qualification** run `35335992785`; and
- **Native resize qualification** run `35335992674`.

All five workflows completed successfully. This commit is the qualified
implementation baseline immediately before the documentation/comment-quality
pass. Documentation-only successor commits do not retroactively extend this
evidence to changed executable source; any later implementation change requires
its own applicable qualification.


### Resize

Online resize is implemented and independently mounted-qualified.

Exact source `c1dd5229e9c42e01ba2d9ab93ef79a6d6521e288` passed dedicated **Native resize qualification** run `33818095528`.

The run built the running-kernel module and resize tools, shrank a 256 MiB filesystem to 128 MiB, verified committed geometry, wrote and hash-recorded live data, grew back to 256 MiB, wrote additional data, refused an unsafe 64 MiB shrink with live allocation beyond the requested boundary, preserved geometry after refusal, unmounted, scrubbed CLEAN, remounted read-only and reverified both data hashes.

This evidence does not depend on the quota gate and does not include the manual million-file/1 TiB or endurance workloads.

### Quotas

Native user/group/project quotas are implemented and mounted-qualified.

Exact cleaned source `b078749efaf7bc76443d7c3dfd82a2c2bfcd6c38` passed the ordinary **Native Linux kernel module** run `33977172394`, including its native user/group/project quota qualification step. The same exact source also passed **Build and conformance** run `33977172446`, including Linux full suite, Clang, ASan/UBSan, GCC static analysis, native Linux package construction and Windows interoperability.

The immediately preceding quota implementation/test content commit `4505fe828718bfa00467cc711b9147bf967f1890` had already passed the complete mounted quota contract in one-shot final quota repair run `33977009426`, job `101335375795`. That run built the exact running-kernel module and tools from the repaired tree, exercised the mounted contract, committed the qualified kernel/test content, and removed all temporary diagnostic machinery. The later ordinary Native Linux pass on `b078749efaf7bc76443d7c3dfd82a2c2bfcd6c38` independently confirms the permanent cleaned tree.

The mounted qualification covers user byte hard limits and release after truncate; user object limits with same-inode hard links not double charged; group object limits; atomic ownership-transfer rejection when the destination quota would overflow; project-root inheritance; project byte/object limits; reflink logical-byte accounting; same-project hard links with subsequent writes still quota-enforced; cross-project hard-link rejection; cross-project rename preflight; project reassignment preflight; project-root deletion/replacement accounting; durable quota rules and project roots; remount usage reconstruction from authoritative objects including multiply-linked files; and final CLEAN scrub verification.

The hard-link reconstruction fix resolves multiply-linked file project ownership from every directory alias because such files intentionally have no single persistent `parent_id`. All aliases must resolve to one project and the observed alias count must agree with persistent `link_count`; disagreement fails closed as corruption. Internal Linux SYSTEM sidecars are excluded from quota accounting before recursive quota capture, preventing quota-policy persistence/eviction from self-deadlocking.

Earlier source `c5dd0bdb063faff4a94579b8a209b4a1e494191b` remains useful historical evidence of the original unresolved hang: Build and conformance run `33805398475` passed, while Native Linux run `33805398514` timed out in the mounted quota step. That historical timeout is superseded for feature-completion purposes by the later successful mounted qualifications above; it is not rewritten as a pass.

## Milestone evidence

| Date | Exact source | Evidence | Result |
| --- | --- | --- | --- |
| 2026-08-29 | `075aed9c737fb38cc408d752736a97773dc2a035` | Full checked-roadmap audit plus destructive physical native-VFS qualification | 69/69 physical checks passed, additional concurrency rounds passed, final scrub CLEAN with zero checksum/metadata errors. |
| 2026-08-30 | `a40a9a9a8f12b4789c3e582b92abf5678a07e79e` | Hosted scale/endurance/online-defrag qualification | Million-file/1 TiB, near-full mixed-workload endurance and mounted defragmentation milestones passed. |
| 2026-08-31 | `b9700642e8eb70c7969124a7add21429608f5003` | Build run `33380655074`; Native run `33380655054` | Pinned libblockdev/UDisks/GNOME Disks integration passed; native parallel allocation qualification demonstrated overlapping reservations and CLEAN scrub. |
| 2026-08-31 | `a4eb8ffe4c47ef36e256a0efa617ff19f7b61985` | Build/Windows run `33333450633` | Driverless ProjFS bridge passed external-client hydration and Windows create/write/rename/hard-link/delete persistence qualification. |
| 2026-09-01 | `69c66ad7fa28b9308729f0181c270cd78a94ea59` | Heavy run `33448454593`; Native run `33448866530` | Workload-aware locality/best-fit placement exercised under near-full/endurance and mounted native qualification. |
| 2026-09-01 | `bf123d2cc98aef8b848386c4f26cc93a04fe4dc3` | Native run `33454396842`; Heavy run `33454248279` | Rotational and non-rotational media-placement policies exercised; images scrubbed CLEAN; endurance passed. |
| 2026-09-01 | `ba1b06561de13dfa434b5cebf2e5797022d83572` | Build run `33474719972`; Native run `33474719970` | Windows bridge small-edit write-back regression and native mounted suite passed. |
| 2026-09-03 | `c1dd5229e9c42e01ba2d9ab93ef79a6d6521e288` | Resize run `33818095528` | Independent mounted online shrink/grow, unsafe-tail refusal, remount verification and CLEAN scrub passed. |
| 2026-09-03 | `c5dd0bdb063faff4a94579b8a209b4a1e494191b` | Build run `33805398475`; Native run `33805398514` | Build passed; mounted quota qualification timed out on this historical source. |
| 2026-09-06 | `4505fe828718bfa00467cc711b9147bf967f1890` | Mounted quota run `33977009426`, job `101335375795` | Full native user/group/project quota contract passed, including hard-link-safe project accounting, remount reconstruction and final CLEAN scrub. |
| 2026-09-06 | `b078749efaf7bc76443d7c3dfd82a2c2bfcd6c38` | Build run `33977172446`; Native run `33977172394` quota step | Permanent cleaned tree passed ordinary conformance and ordinary mounted quota qualification. |
| 2026-09-06 | `30c26c6ede56c9c72999db6c03bd04e15c0047e1` | Build run `33988573241`; Native run `33988573153` | Final Format 0.18 alias-fixture repair passed ordinary conformance and the complete native mounted suite, including quotas, read/write and scrub qualification, online grow/shrink, media-aware placement and online defragmentation. |

Detailed step logs and performance telemetry remain in the corresponding GitHub Actions runs and Git history rather than being copied into multiple documentation files.

## Evidence rules

1. A checked roadmap capability needs implementation plus the qualification appropriate to that capability.
2. Portable/build CI, mounted native qualification, heavy stress and destructive physical-media qualification are distinct evidence classes.
3. A failure or timeout in an ordered mounted gate prevents later skipped steps from being claimed for that exact source.
4. Long qualification must remain manual-only when it intrinsically exceeds 10 minutes; it must not be quietly reintroduced as ordinary push, scheduled or per-release work.
5. Release publication must never treat a skipped mounted automatic qualification as equivalent to a mounted pass.
6. Exact run IDs, commit hashes and historical metrics belong here, not in the README, ROADMAP or architecture documents.
7. Workflow YAML is executable policy. If this ledger disagrees with the workflows, fix the disagreement rather than maintaining two competing descriptions.

### Linux system-root qualification

The real UEFI root-boot workflow constructs an EFI + ext4 `/boot` + InfiltratorFS `/` VM, reaches systemd with InfiltratorFS as `/`, exercises metadata and dpkg workloads, installs a newer InfiltratorFS package while the root is live, rebuilds initramfs, reboots, forces power loss during writes, requires an offline CLEAN scrub, boots the same root again, verifies dpkg and metadata state, and requires a final CLEAN scrub.

Because this qualification intentionally exceeds 10 minutes, it is manual-only. Run it deliberately for changes that materially affect root mounting, boot/initramfs, package upgrade semantics, crash recovery, checkpoint recovery or similarly high-risk paths. Historical root-boot evidence remains valid only for the exact source on which it ran; ordinary fast CI does not silently inherit it.

### 0.18.94 per-object protection/encryption qualification

Format 0.18 per-object storage policy is qualified at the portable-storage and
filesystem layers. The policy contract persists a protection-copy count and
encryption-domain ID with the object, keeps nondefault-policy file data out of
inline metadata, and fails closed when a backend cannot honor a requested
semantic.

Qualification includes:

- deterministic one-copy and two-copy placement across mirror members;
- domain-separated AES-256-GCM read/write and authentication failure under the
  wrong domain;
- composed mirror/encryption wrappers without silently discarding one policy
  dimension;
- no replica quarantine for semantic policy errors;
- end-to-end protected file creation, reopen/read, named-stream inheritance,
  selected-member failure behavior and scrub;
- protected small-file and truncate behavior that remains extent-backed; and
- ordinary presentation-flag mutation without changing the storage policy.

GitHub Actions build-and-conformance run `36221054899` on source
`f967b5c33fc9e4be48c229cd60de5d7abaaa7f66` passed the Linux full suite,
Clang conformance and ASan/UBSan test suites, including the policy tests. The
Windows build also compiled the same portable core and passed
`infilfs-storage-mirror`, `infilfs-storage-encrypted`,
`infilfs-protected-volume` and `infilfs-inline-files`; two unrelated Win32
storage-backend callback-layout regressions were identified separately and
subsequently repaired with designated initializers.

### Operator physical-power-loss evidence preparation

`tests/native-powerloss-witness.sh` provides a bounded independent-witness
exercise on an already mounted dedicated native physical volume and a separate
physical witness disk on a dedicated test machine. It does not format devices
or initiate power cuts.

```sh
bash tests/native-powerloss-witness.sh write /mnt/infilfs-test /mnt/independent-witness trial-01
# After an operator-controlled abrupt power interruption and recovery:
bash tests/native-powerloss-witness.sh verify /mnt/infilfs-test /mnt/independent-witness trial-01
```

The writer generates and hashes data independently, then flushes each payload
and its directory before publishing a hash record on the witness disk. A witness
record is only acknowledged after its own flush. Verification requires a different
boot ID and checks every contiguous record's exact length and SHA-256. Unwitnessed
trailing writes are not claimed durable. Both directories are retained as evidence.
A normal reboot can also satisfy the boot-ID gate; record the actual hardware
interruption, controller/cache configuration and device identity independently.
Repeat at different mutation and publication phases; perform offline structural
fsck and deep scrub after recovery, and retain exact-source logs. A bounded witness
pass does not replace full native endurance or establish production qualification.

### Current Common V2 firmware dependency boundary

The BOOT repository was rebuilt after the historical Common V2 integration.
Its current released v0.1.642 package has the native EFI-driver design rather
than the former generic Common V2 ELF loader. The old v0.1.50 tag and package
were not reliable references to the previous integration. The firmware gate
pins the current released package and verifies its digest and expected package
layout. Absence of the required configuration token is a dependency failure,
not a provider conformance pass. Restore and qualify BOOT's generic loader,
then pin a compatible immutable release and execute the real QEMU/OVMF
provider/kernel handoff. Standalone EFI-driver boot and hosted module tests do
not substitute for this gate.

### Accidental-format resilience qualification (planned)

No pass is recorded. Compare alternative designs using disposable known-manifest images containing files, directories, hard links, sparse/shared/compressed data, attributes, ACLs and snapshots. Record exact source, formatter versions, actual overwrite/discard ranges, geometry and recovered-versus-expected content and metadata.

- Verify zero target writes for refused/ambiguous format requests, wrong target identity, busy/mounted devices and insufficient retention space.
- Inject interruption and torn writes at each native recoverable-format preparation, publication and release boundary; verify old-or-new authoritative state, prior-graph pinning, no discard of retained blocks, identity separation and scrub.
- Exercise actual ext4, NTFS and exFAT quick formatters on disposable images; independently overwrite partition tables, anchor regions and essential metadata; test progressive subsequent allocation and complete-overwrite/discard negative controls. Document actual formatter behaviour rather than assuming a universal quick format.
- Verify source byte identity after read-only scan/export; reject false verified recoveries and cross-volume or cross-generation graph mixing. Compare paths, contents and metadata with manifests. Test any in-place restoration for interruption safety.
- Test wrong/missing keys, damaged recovery/key records, malformed media, divergent replicas, near-full and resized volumes, secure purge, and bounded resource usage.
- Measure capacity and write-amplification overhead, recovery scan time and fidelity on representative HDD, SSD and NAS-like geometries. Physical power-loss, endurance and device discard behaviour require separate hardware evidence.

Supported native recoverable-format cases require complete manifest fidelity; foreign-format cases require accurate reporting of verified, partial and unrecoverable data, not a universal success percentage. Architecture selection must consider these results before freezing a stable format.

---

## Proposed Format 0.20 — unified design (revisions 3–4)

**Proposal only; the preceding current-format contract remains applicable to Format 0.19.** The [unified Format 0.20 specification](ON_DISK_FORMAT.md#proposed-infiltratorfs-on-disk-format-020) is the design authority for the proposed persistent model. This section specialises that model; it does not introduce a second on-disk authority or claim implementation.

This is a planned qualification contract, not a pass record. The detailed acceptance matrix is in the proposed format specification. Before schema freeze, model bootstrap/ownership cycles, durable reference updates, stable location relocation, semantic commitment reconstruction, transaction fault states and encryption/member authority. Before completion, run exact-source portable/native/Windows/Common V2 vectors and actual-writer fault injection, not just abstract models. Compare independent manifests across foreign quick formats, lost primary structures, mixed recovery copies, native repeated format/unformat, compression, reflinks, snapshots, security and encryption; record precise write/discard maps and separate first-mount initialisation. Require complete fidelity where all committed essential records, payloads and keys survive, explicit loss classification otherwise. Measure real 0.19 and comparable filesystem baselines with equivalent durability/protection configuration; report latency percentiles, throughput, memory, metadata overhead, write amplification and scale including near-full workloads. Physical power interruption, endurance, Windows mounted/Driver Verifier and BOOT/native root boot remain distinct evidence classes. Ten-minute automatic CI cap remains; long/destructive tests are manual. No prior qualification or historical benchmark is promoted to 0.20.

### Revision 4 — explicit candidate verification
See ON_DISK_FORMAT.md sections 20.13–20.19 for golden envelope/record vectors, transaction and reclaim transition matrix, independent ownership tracing, recovery proof and staged acceptance. Test loss of all copies of a required proof node as a negative control: do not claim full committed reconstruction merely from valid-looking leaves. Measure actual foreign writes/discards, baseline latency percentiles and write amplification, metadata/checksum overhead and cache memory. The numerical digest/envelope overhead examples are arithmetic estimates, not empirical results. No new model, prototype or physical test is recorded as passed by this documentation revision.

### Format 0.20 S0 candidate conformance (unqualified)
The private `infilfs-format-020-schema` target registers a CTest for the experimental envelope and framing. A fixed independent SHA-256/CRC64 vector and negative cases are included. No execution or filesystem qualification is asserted by this change; consult exact-source CI or a recorded local run. Existing 0.19 evidence is unaffected.

### Format 0.20 S0/S1 bounded executable store (unqualified at commit)
The registered `infilfs-format-020-store` CTest drives the actual experimental writer and cold reader through injected refusal, torn/persisted writes and flush failures; it also tests lane and mixed-copy loss, verified healing, absent proof and preflight. This is a bounded in-memory storage model, not production-media or native filesystem qualification. Record exact-source execution separately. The existing schema vector is independently verified by the literal encoded preimage; no production claims follow.

### Format 0.20 S2 ownership model (execution pending)
The isolated `infilfs-format-020-ownership` test persists and reloads root retirement/reactivation, reference-count transitions and one-edge deferred reclamation using the S1 tree. Invalid counts, overlapping protected ranges and cycles must produce zero writes. The model tests explicit root pins, not the production self-hosting allocator, payload writes, automatic fallback retirement or native formatting. No new pass is asserted here.

### Format 0.20 physical-payload integration model (execution pending)
The registered infilfs-format-020-volume CTest covers real block writes through the existing experimental callback interface, old-or-new publication after injected failure, verified content and shared relocation, snapshot and native-format reversal, two-lane loss and missing/corrupt payload refusal. This is simulated durable storage, not physical-media qualification or a production filesystem. No pass is asserted here.
The extended volume CTest also exercises raw-byte discovery of a displaced original region after loss of two metadata lanes, ambiguous duplicate regions, missing commit proof and byte-preserving read-only reconstruction. No physical formatter or production Unformatter qualification is asserted.

### Foreign formatter image characterization (execution pending)
The non-installed POSIX test target invokes an actual formatter on a private generated regular-file copy, emits observed changed-block ranges and independently checks proof/payload survival against the recovery result. This is a real formatter experiment on a file image, not a physical-device trace or proof of production layout. The ext4 CTest is conditional on the real executable being present. Other explicit profiles are documented in the candidate specification. The local Linux workflow now runs all registered format-020 tests verbosely. No result is claimed without an exact-source run.

### 25 November 2026 — verified experimental Format 0.20 integration baseline

Exact source: `d67cdb9ca7c66dbac60ea61fe639a593e0bb2076`. The [local Linux userspace job](https://github.com/Infiltrator-Projects/InfiltratorFS/actions/runs/45526949976/job/138912582079) built the targets and executed the five registered Format 0.20 tests. All five passed. The existing cross-platform image smoke also passed in this job.

| Test | Recorded result |
|---|---|
| schema | PASS, experimental envelope and record framing |
| store | PASS, 66 actual experimental writer interruption cases and lane/mixed-copy recovery |
| ownership | PASS, persistent root retention, shared edges and bounded reclaim |
| volume | PASS, 42 payload/metadata interruption cases, shared relocation, native undo, raw region discovery and ambiguity refusal |
| foreign-ext4 | PASS, real formatter characterization on a generated 64 MiB regular file |

The ext4 command was `/usr/sbin/mkfs.ext4 -t ext4 -F -E nodiscard,lazy_itable_init=1,lazy_journal_init=1` against the private generated copy. The 16 changed 4 KiB blocks were in the half-open block ranges [0,3), [9,16), [25,26), [41,42), [1065,1066) and [16374,16377). Each of the three latest-generation proof slots (commit, root, leaf) retained two byte-identical copies. Raw discovery selected sequence 6; original, clone and later (retained pin 7) all read with verified contents. This is 100% fidelity for this three-name test dataset, not a general recovery-rate estimate. These ranges describe observed final changed blocks, not the formatter's complete write/discard trace. First mount, discard-enabled media, actual Windows format, other geometry/profile combinations, larger/fragmented datasets and production scalability remain unqualified.

The prior Clang run on `d2e11d217280f4a5cf4aed6e893848a7c9442a58` exposed a test assertion confusing a transaction ID with its committed sequence. The assertion was corrected in the above verified revision; no recovery semantics were weakened. The separate BOOT job at the prior revision reported the pinned BOOT v0.1.642 lacks the filesystem_module configuration token required by Common V2. This is an outstanding integration gate, not evidence that the experimental filesystem is bootable. No native 0.20 mount, production schema freeze, physical power-loss or release qualification is claimed by these tests. The current production format remains 0.19.

### Multi-unit allocation and partial-recovery model (execution pending)
The new private `infilfs-format-020-multiblock` CTest exercises multi-unit dependency publication and relocation at injected write/flush failures, shared data mappings, native undo, loss of two metadata lanes, corrupt versus unreadable unit reporting, verified range reads, no source mutation and complete-run reservation. Earlier single-block tests remain registered. No pass is asserted before exact-source execution.

### 25 November 2026 — multi-unit model execution
The [local Linux userspace job](https://github.com/Infiltrator-Projects/InfiltratorFS/actions/runs/45533944845/job/138935818267) at exact source `a2e607224e6cd3fac7f918c09edc95b6dae4d237` executed all six Format 0.20 tests successfully: schema, store (66 interruption cases), ownership, volume (42 cases), multiblock (96 write/relocation cases), and the actual ext4 regular-file experiment. This is bounded experimental evidence only.

### Variable-height tree candidate (execution pending)
The registered `infilfs-format-020-tree` test exercises multi-level insert/lookup/delete, immutable retained roots, bounded path copying and mixed replica recovery. Private write errors must leave the previous root usable. This module has not yet been integrated with persistent allocation or commit publication; no new pass or mounted-format claim is asserted.

### Paged tree and commit integration (execution pending)
The `infilfs-format-020-paged-store` target tests actual staged tree writes and bootstrap publication under refusal, torn/persisted writes and flush failures; two-lane loss, displaced-region full proof, missing proof and zero-write arena exhaustion. This remains an experimental fixed-geometry append profile. No new test pass is asserted before exact-source CI.

### 25 November 2026 — variable-height tree execution
The [local Linux userspace job](https://github.com/Infiltrator-Projects/InfiltratorFS/actions/runs/45534244560/job/138936945614) at exact source `479fb2668e6474df26fd93f9d77a53ebad1b6354` passed all seven experimental tests. The new tree test inserted 96 keys, reached six levels and passed 18 private-write failure cases. The test covers path copying, historical roots and missing proof, not mounted filesystem qualification.

### Atomic batch conformance (execution pending)
The registered batch test checks multi-record and payload publication as one authority, first-transaction creation, late missing-key zero-write refusal and every simulated write/flush failure. No pass is asserted until exact-source execution.

### Typed paged-volume integration (execution pending)
The new `infilfs-format-020-paged-volume` test runs the existing file/ownership model through the authenticated variable-height tree and common batch commit, checking actual payload/metadata interruptions, shared integrity, native root reversal, two-lane loss and read-only exact unit loss. No pass is asserted before exact-source CI.

### Experimental image export (execution pending)
The Linux export test creates a private paged image and exercises the actual command, verified original names, explicit partial fragments and source immutability; no pass is asserted before exact-source CI. This is not a production-media recovery qualification.

### 25 November 2026 — integrated experimental paged filesystem and image export

Exact source `ce9dce04e9d3dabcd9243f5198d0f103c4858441`: [Linux userspace qualification](https://github.com/Infiltrator-Projects/InfiltratorFS/actions/runs/45534978695/job/138939381403) passed all eleven dedicated Format 0.20 CTests. The reported injected interruption/failure counts are 66 store, 42 volume, 96 multi-block, 18 tree, 84 paged store, 78 batch, and 147 typed paged-volume cases (531 in total). Schema, ownership, image export and the ext4 regular-file experiment also passed. The export test verifies two original named copies, a shared allocation with one corrupted unit, exact verified fragments, refusal to overwrite the destination, and source immutability. A complete export describes the selected surviving commit only; it is not proof that no newer commit was destroyed. Treat a failed or interrupted exporter as incomplete even if output files or a result record are present.

The [preceding integrated-volume run](https://github.com/Infiltrator-Projects/InfiltratorFS/actions/runs/45534738877/job/138938582844), source `12eefc1a87f62626c03a5cdc57117b26d72edc96`, passed all ten then-registered tests. Its Clang conformance check also passed. Its [BOOT job](https://github.com/Infiltrator-Projects/InfiltratorFS/actions/runs/45534738926/job/138938449400) explicitly failed because pinned BOOT v0.1.642 lacks the generic Common V2 `filesystem_module` configuration token. Do not weaken that gate or infer boot qualification.

This is experimental evidence, not a release qualification. Contiguous bounded payloads, append-only arena allocation, bounded flat namespace, full-model reconstruction/diff, incomplete general reclamation and absent production native integration remain explicit limits. A new production media identifier, native mount or formatter release is not authorized. Other check conclusions must be obtained from the respective exact-source workflow; no whole-suite, physical-device, Windows, encryption or replica qualification follows from these eleven tests.

### Payload retirement and reuse (execution pending)
The registered format-020-reclaim test uses two contiguous payload gaps, explicit snapshot release, collection, a successor commit and reuse of the original physical range. It exercises actual writer interruption with torn and persisted-error modes and checks prior/new authority and the unaffected live file. No result is claimed without exact-source execution. Metadata arena reclamation and physical media qualification remain outstanding.

### Metadata arena reuse (execution pending)
The registered format-020-arena-gc test exercises a two-slot arena with no append capacity remaining, unchanged-root collection, injected failures before/after each writer barrier, reuse and 32 repeated cycles, and no-write refusal with missing tree proof. The test is an executable bounded model; no pass is asserted until exact-source CI.
