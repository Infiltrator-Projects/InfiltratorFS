# InfiltratorFS fsck / scrub separation

Status: implemented on `main`; release qualification in progress for 0.18.54.

## Incident that exposed the defect

The previous Linux `fsck.infiltratorfs` helper was not a filesystem checker. It directly launched `infilfs-scrub`, so a normal fsck request became an exhaustive full-volume scrub.

This caused an hours-long scan during a format-and-clone workflow in which the existing data was disposable and was about to be destroyed. That wasted substantial user time and compute cost. The behaviour was not merely a performance problem: the maintenance command semantics were wrong.

This document was committed before the implementation changes so the correction and the reason for it remain part of the project record.

## Command contract

`fsck.infiltratorfs <device>` is the primary filesystem maintenance command. Its default operation is a fast structural consistency check comparable in purpose to CHKDSK/fsck. It does not read every user-data block or recompute every file-data checksum.

The default check validates, as applicable:

- readable and internally valid superblock/checkpoints;
- allocation metadata structure and accounting;
- object index structure and object references;
- root object and namespace structure;
- directory/link/reference consistency;
- file extent metadata and bounds;
- checksum metadata structure and ownership, without reading all file payload data;
- snapshot catalogue/metadata structure without deep-reading snapshot file payloads;
- other format invariants necessary to decide whether the filesystem metadata is structurally clean.

Its output is explicit and human-readable, identifying the major checks performed and ending with a clear CLEAN / ERROR result. On a newly formatted empty filesystem it should complete essentially immediately.

`fsck.infiltratorfs --scrub <device>` is the explicit heavyweight mode. It invokes the authoritative scrub and may perform exhaustive file-data reads, checksum recomputation, retained-generation verification and other deep integrity work. A potentially hours-long operation must never be selected implicitly by ordinary `fsck.infiltratorfs` invocation.

`infilfs-scrub` remains available as the dedicated forensic/deep-integrity utility for qualification, suspected latent corruption, periodic data verification and development testing.

## Compatibility and boot policy

The standard Linux fsck helper and initramfs integration use the fast structural check by default. Boot-time filesystem checking must not silently perform a complete data scrub. Deep scrub remains opt-in only.

Existing conventional fsck flags (`-a`, `-f`, `-n`, `-p`) continue to be accepted where required by Linux integration. Until repair semantics are implemented, the checker remains fail-closed/read-only and reports corruption rather than pretending it repaired anything.

## Qualification contract

Tests enforce that:

1. plain `fsck.infiltratorfs` does not execute `infilfs-scrub`;
2. `fsck.infiltratorfs --scrub` does execute the deep scrub path;
3. a freshly formatted empty filesystem passes the fast check;
4. structural metadata corruption is rejected by the fast check;
5. file-data checksum corruption that leaves metadata structurally valid is reserved for `--scrub` detection rather than forcing ordinary fsck to read the complete payload;
6. initramfs packages both required tools but invokes the fast checker by default;
7. packaging and installed-system tests enforce the command contract so the old alias cannot silently return.

This separation is a product requirement, not an implementation detail.