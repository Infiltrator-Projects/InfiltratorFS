<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Qualification Ledger

This file is the **single authoritative exact-source evidence record** for InfiltratorFS. It records what was actually exercised on named commits and workflows. It is not a feature list; feature completion belongs in `ROADMAP.md`.

Historical results apply only to the source commit on which they ran. A later green portable CI run does not silently inherit an older mounted, heavy or destructive qualification result.

## Qualification classes

- **Build and conformance** — ordinary broad CI: GCC/Linux, Clang, sanitizers, static analysis, Windows portability/interoperability, policy guards and package construction.
- **Native Linux kernel module** — mounted kernel qualification for relevant kernel/core/package changes. It builds the module/DKMS source and, when the running-kernel environment is available, exercises native mounted behaviour.
- **Native resize qualification** — dedicated mounted grow/shrink qualification independent of the quota gate.
- **Heavy filesystem qualification** — million-file/1 TiB scale plus near-full mixed-workload endurance. This is weekly/manual milestone evidence, not ordinary per-push CI and not an automatic prerequisite for every release.
- **Formatter integration qualification** — pinned libblockdev/UDisks/GNOME Disks integration build and end-to-end formatter/probe checks.
- **Physical partition qualification** — explicitly destructive operator-run qualification on dedicated media; never unattended ordinary CI.
- **Release publication gate** — requires successful same-source release prerequisites, then installs the generated package, verifies native filesystem registration, mounts a real Format 0.17 image, performs non-zero write/read verification, syncs, unmounts and requires a CLEAN scrub. It also rejects restoration of the legacy FUSE product path.

A workflow should fail closed when a qualification class claims mounted coverage but the required running-kernel environment is unavailable. Merely skipping mounted work must not be treated as equivalent evidence.

## Current development evidence boundary

### Resize

Online resize is implemented and independently mounted-qualified.

Exact source `c1dd5229e9c42e01ba2d9ab93ef79a6d6521e288` passed dedicated **Native resize qualification** run `33818095528`.

The run built the running-kernel module and resize tools, shrank a 256 MiB filesystem to 128 MiB, verified committed geometry, wrote and hash-recorded live data, grew back to 256 MiB, wrote additional data, refused an unsafe 64 MiB shrink with live allocation beyond the requested boundary, preserved geometry after refusal, unmounted, scrubbed CLEAN, remounted read-only and reverified both data hashes.

This evidence does not depend on the quota gate and does not include the weekly/manual million-file/1 TiB or endurance workloads.

### Quotas

Native user/group/project quotas are implemented and mounted-qualified.

The quota implementation/test content committed as exact source `4505fe828718bfa00467cc711b9147bf967f1890` passed the complete mounted quota qualification in one-shot final quota repair run `33977009426`, job `101335375795`. The run checked out the immediately preceding deterministic repair input, applied the recorded quota transform, built the exact running-kernel module and tools from that resulting tree, exercised the complete mounted quota contract, then committed that qualified kernel/test content as `4505fe828718bfa00467cc711b9147bf967f1890` and removed the temporary diagnostic machinery.

The mounted qualification covered user byte hard limits and release after truncate; user object limits with same-inode hard links not double charged; group object limits; atomic ownership-transfer rejection when the destination quota would overflow; project-root inheritance; project byte/object limits; reflink logical-byte accounting; same-project hard links with subsequent writes still quota-enforced; cross-project hard-link rejection; cross-project rename preflight; project reassignment preflight; project-root deletion/replacement accounting; durable quota rules and project roots; remount usage reconstruction from authoritative objects including multiply-linked files; and final CLEAN scrub verification.

The hard-link reconstruction fix resolves multiply-linked file project ownership from every directory alias because such files intentionally have no single persistent `parent_id`. All aliases must resolve to one project and the observed alias count must agree with persistent `link_count`; disagreement fails closed as corruption. Internal Linux SYSTEM sidecars are excluded from quota accounting before recursive quota capture, preventing quota-policy persistence/eviction from self-deadlocking.

Earlier source `c5dd0bdb063faff4a94579b8a209b4a1e494191b` remains useful historical evidence of the original unresolved hang: Build and conformance run `33805398475` passed, while Native Linux run `33805398514` timed out in the mounted quota step. That historical timeout is superseded for feature-completion purposes by the later successful mounted qualification above; it is not rewritten as a pass.

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

Detailed step logs and performance telemetry remain in the corresponding GitHub Actions runs and Git history rather than being copied into multiple documentation files.

## Evidence rules

1. A checked roadmap capability needs implementation plus the qualification appropriate to that capability.
2. Portable/build CI, mounted native qualification, heavy stress and destructive physical-media qualification are distinct evidence classes.
3. A failure or timeout in an ordered mounted gate prevents later skipped steps from being claimed for that exact source.
4. Heavy qualification remains weekly/manual unless project policy is deliberately changed; it must not be quietly reintroduced as ordinary per-release work.
5. Release publication must never treat a skipped mounted qualification as equivalent to a mounted pass.
6. Exact run IDs, commit hashes and historical metrics belong here, not in the README, ROADMAP or architecture documents.
7. Workflow YAML is executable policy. If this ledger disagrees with the workflows, fix the disagreement rather than maintaining two competing descriptions.
