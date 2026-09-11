<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Qualification Ledger

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
