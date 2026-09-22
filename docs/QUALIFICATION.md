<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Qualification Ledger

The forensic cross-check of current kernel invariants against the 2026
filesystem research/advisory failure modes is recorded in
[`FILESYSTEM-PAPER-AUDIT-2026.md`](FILESYSTEM-PAPER-AUDIT-2026.md).

This file is the **single authoritative exact-source evidence record** for InfiltratorFS. It records what was actually exercised on named commits and workflows. It is not a feature list; feature completion belongs in `ROADMAP.md`.

Historical results apply only to the source commit on which they ran. A later green portable CI run does not silently inherit an older mounted, heavy or destructive qualification result.

## Qualification classes

- **Build and conformance** — ordinary broad CI: GCC/Linux, Clang, sanitizers, static analysis, Windows portability/interoperability, policy guards and package construction. Every automatic job is capped at 10 minutes.
- **Native Linux kernel module** — mounted kernel qualification for relevant kernel/core/package changes. It builds the module/DKMS source and, when the running-kernel environment is available, exercises native mounted behaviour.
- **Native resize qualification** — dedicated mounted grow/shrink qualification independent of the quota gate; automatic execution is capped at 10 minutes.
- **Heavy filesystem qualification** — million-file/1 TiB scale plus near-full mixed-workload endurance. This is manual-only milestone evidence because it intentionally exceeds the automatic CI ceiling.
- **Real root-boot qualification** — real UEFI boot, live-root package upgrade, forced power-loss recovery and repeated scrub. This is manual-only and is run when root/boot/recovery risk warrants it.
- **Formatter integration qualification** — pinned libblockdev/UDisks/GNOME Disks integration build and end-to-end formatter/probe checks. The full upstream-stack build is manual-only because it intentionally exceeds the automatic CI ceiling.
- **Physical partition qualification** — explicitly destructive operator-run qualification on dedicated media; never unattended ordinary CI.
- **Release publication gate** — requires successful same-source automatic release prerequisites, then installs the generated package, verifies native filesystem registration, mounts a real Format 0.18 image, performs non-zero write/read verification, syncs, unmounts and requires a CLEAN scrub. It also rejects restoration of the legacy FUSE product path. Long manual qualifications are supporting evidence, not mandatory automatic release blockers.

A workflow should fail closed when a qualification class claims mounted coverage but the required running-kernel environment is unavailable. Merely skipping mounted work must not be treated as equivalent evidence.

## CI duration policy

Automatic qualification and conformance jobs are hard-capped at 10 minutes where they are part of the ordinary CI set. A test that intrinsically needs more than 10 minutes must be a manual-only qualification and is launched deliberately when its coverage is warranted. The current long-running manual set is the real UEFI/root crash-recovery qualification, the full GNOME/libblockdev/UDisks formatter build, and the million-file/1 TiB plus endurance qualification.

## Current development evidence boundary

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
