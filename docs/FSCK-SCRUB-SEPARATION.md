# InfiltratorFS fsck / scrub separation

Status: implementation correction in progress after 0.18.54 exposed an incomplete consolidation.

## Incident that exposed the defect

The original Linux `fsck.infiltratorfs` helper was not a filesystem checker. It directly launched the standalone deep scrub command, so a normal fsck request became an exhaustive full-volume scrub.

This caused an hours-long scan during a format-and-clone workflow in which the existing data was disposable and was about to be destroyed. That wasted substantial user time and compute cost. The behaviour was not merely a performance problem: the maintenance command semantics were wrong.

The first 0.18.54 correction separated the default fast check from `--scrub`, but it still implemented `fsck.infiltratorfs` as a wrapper around separate executables. In particular, `fsck.infiltratorfs --scrub` still launched `/usr/bin/infilfs-scrub`. That was an incomplete implementation of the intended design and would fail as soon as the redundant standalone scrub command was removed.

The corrected design below is the product contract.

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

`fsck.infiltratorfs --scrub <device>` is the explicit heavyweight mode. The same native `fsck.infiltratorfs` executable directly calls the authoritative scrub APIs and performs the full data-integrity scan. It does not execute, wrap, depend on, or delegate to another scrub command.

The deep mode retains the complete former scrub functionality, including ordinary offline scrub, stable online scrub (`--scrub --online`) and named snapshot scrub (`--scrub --snapshot <name>`).

There is no standalone installed `infilfs-scrub` command. InfiltratorFS is pre-1.0 and has no compatibility requirement to preserve a redundant command name.

## Compatibility and boot policy

The standard Linux fsck helper and initramfs integration use the fast structural check by default. Boot-time filesystem checking must not silently perform a complete data scrub. Deep scrub remains opt-in only through `fsck.infiltratorfs --scrub`.

Existing conventional fsck flags (`-a`, `-f`, `-n`, `-p`) continue to be accepted where required by Linux integration. Until repair semantics are implemented, the checker remains fail-closed/read-only and reports corruption rather than pretending it repaired anything.

## Qualification contract

Tests must enforce that:

1. `fsck.infiltratorfs` is a native executable linked to the filesystem core rather than a shell wrapper;
2. plain `fsck.infiltratorfs` performs the structural check without reading all user payload data;
3. `fsck.infiltratorfs --scrub` directly executes the full scrub implementation through the core scrub APIs;
4. no installed or built standalone `infilfs-scrub` utility is required for either mode;
5. a freshly formatted empty filesystem passes the fast check;
6. structural metadata corruption is rejected by the fast check;
7. file-data checksum corruption that leaves metadata structurally valid is reserved for `--scrub` detection rather than forcing ordinary fsck to read the complete payload;
8. initramfs packages only the unified fsck executable for filesystem checking;
9. Manager, qualification scripts and packaging invoke `fsck.infiltratorfs --scrub` for explicit deep verification.

This separation and consolidation are product requirements, not implementation details.
