# Changelog

This file records user-visible, compatibility, architecture and validation changes for InfiltratorFS.

## Unreleased

No unreleased changes are recorded after the 0.18.60 release candidate.

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
