<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS On-Disk Format 0.4 — Part 4

Commit ordering is:

```text
write new/unreachable metadata and newly allocated data
fsync
write new bitmap image at a new physical location
fsync
write one generation N+1 checkpoint
fsync                         <- atomic publication point
write the same checkpoint to the other two physical copies
fsync
```

The first checkpoint chosen for publication rotates by generation. If power is lost before its durable write, mount selects generation `N`. If power is lost after it, mount selects `N+1`. The remaining older checkpoint copies are still physically valid because superseded blocks were not reused during the interrupted transaction. Before a writable mount allocates again, it heals all checkpoint locations to the newest fully validated generation.

This is a no-replay transaction design: there is no journal whose operations must be replayed to discover the committed namespace. The checkpoint itself identifies the committed root/index/bitmap generation.

### File-data publication

Format 0.4 extends the same publication rule to ordinary file-data overwrites. A data block that belongs to committed generation `N` is not overwritten. The writer allocates an unreachable replacement block, writes the new complete block image, updates the file extent mapping, updates the independently stored checksum object, and only then allows the transaction checkpoint to publish generation `N+1`.

A crash before the first `N+1` checkpoint therefore leaves the old extent mapping and old checksum reachable. A crash after the commit-point checkpoint exposes the replacement data and matching checksum. Blocks allocated earlier within the same uncommitted transaction may be filled in place because generation `N` cannot reach them.

### Crash-injection verification

The test suite contains deterministic failpoints immediately before bitmap publication, immediately after bitmap publication, and immediately after the first checkpoint commit. Tests prove that the first two recover the old generation, the last recovers the new generation, and the subsequent writable open heals all three checkpoint copies before another transaction.

## 13. Corruption rejection

The current opener rejects, among other conditions:

- invalid checkpoint checksums or geometry;
- inconsistent bitmap/free-block accounting;
- reserved metadata blocks marked free;
- invalid root object ID or parent relationship;
- invalid metadata checksums;
- malformed object payload lengths;
- malformed directory record lengths or names;
- duplicate object-index identities;
- index entries pointing outside the volume or to free blocks;
- object/index type or identity mismatches;
- extents with invalid logical ordering or physical bounds;
- malformed checksum-object chains or missing checksum entries when data is read;
- file-data blocks whose calculated checksum differs from the independently stored checksum.

The intent is to fail closed rather than continue with metadata that cannot be trusted.

## 14. Deliberate format-0.4 limits

The following are intentionally deferred rather than hidden:

- one-block object index;
- one-block directories;
- no hard links;
- no symbolic links;
- no sparse extent representation;
- SHA-256 file-data checksums; metadata/checkpoint checksums remain CRC64-ECMA in this revision;
- scrub detects corruption but does not yet repair it because redundant data placement is not implemented;
- no compression, reflinks or snapshots;
- single-writer FUSE operation.
