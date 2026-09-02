<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Compression Design

## Status

Format 0.17 uses adaptive per-extent compression as a normal filesystem
capability. Newly formatted volumes enable `INFS_INCOMPAT_COMPRESSED_EXTENTS`.
The automatic native codec is **IAC1 version 1** (codec identifier 2).

IAC1 is the InfiltratorFS-native bounded codec selected for Format 0.17. LZ4
(codec identifier 1) remains supported as a development/reference and
interoperability codec, but new automatic writes do not select LZ4.

Compression is a storage representation beneath the logical file-data
contract. SHA-256 continues to protect the exact uncompressed logical bytes.

## IAC1 v1

IAC1 is deliberately small, deterministic and suitable for both the portable
core and the native Linux kernel adapter. It uses:

- independently decodable bounded compression units of at most 64 filesystem
  blocks (256 KiB);
- identity, byte-delta and four-byte XOR reversible predictor modes;
- a bounded 64 KiB backward match window;
- deterministic two-candidate hash match finding;
- explicit fill-run tokens for zero-heavy and repeated-byte data;
- bounded literal and match tokens;
- no persistent global dictionary, hidden mutable model or machine-specific
  state; and
- a fixed 128 KiB match-finder scratch structure in the current implementation.

The stream header records the IAC1 version and predictor mode. Unknown versions,
invalid modes, zero-distance matches, malformed fills, truncated streams and
output-length mismatches are rejected.

The current format keeps codec identification in each compressed extent. The
legacy low two codec bits remain unchanged for identifiers 0-3, while previously
unused high flag bits extend the namespace to 11 bits (0-2047). Existing Format
0.17 compressed extents therefore retain byte-for-byte interpretation, while
future codecs can receive new identifiers without changing file identity or
requiring every extent on a volume to use the same representation.

## Adaptive selection policy

Compression is decided per bounded write cluster, not per file and not as a
permanent whole-volume assumption.

Before encoding, a cheap sampler rejects high-entropy input that lacks useful
byte, delta or four-byte repetition. If IAC1 is attempted, the writer compares
the encoded physical block count with the uncompressed physical block count.
The compressed representation is selected only when it consumes fewer
filesystem blocks. Otherwise the data is stored normally.

This means already-compressed media, encrypted data and other incompressible
payloads normally avoid the full compressor and never consume extra data blocks
merely because compression is enabled.

Linux mounted writes apply compression to eligible sequential EOF clusters.
Portable-core writes use the same IAC1 format and selection rules. Persistent
decoding never depends on the volatile workload classifier or media-placement
policy.

## Mutation semantics

A compressed extent is one bounded codec stream. Operations that would slice a
stream first materialize or replace the affected bounded cluster instead of
pretending that its logical blocks map one-for-one to stored physical blocks.

Current compression-aware qualification covers:

- portable and native reads and writes;
- partial overwrite and copy-on-write replacement;
- sparse files and hole punching;
- truncate across compressed-cluster boundaries;
- `fallocate` reservation semantics without accidental compression of
  synthetic reserved space;
- reflinks and shared-extent release accounting;
- snapshots and retained historical generations;
- hard-link/unlink and rename lifetime;
- compact and paged extent maps;
- logical-versus-physical allocation reporting;
- page-cache/mmap read paths;
- scrub and logical SHA-256 verification; and
- corrupted compressed payload rejection.

Online defragmentation currently counts the physical footprint of compressed
extents but deliberately leaves an intact compressed stream in place. Defrag is
not allowed to split a codec stream.

## Qualification policy

`tests/compression-codec.c` provides deterministic known-behaviour tests for
IAC1 predictors, ratio on simple compressible classes, deterministic output,
cheap high-entropy rejection and malformed-stream rejection.

`tests/compression-extents.c` qualifies the actual Format 0.17 extent
representation, adaptive fallback, snapshots, mutation, scrub and corruption
handling.

`tests/compression-qualification.c` is the representative workload gate. It
uses 256 KiB filesystem-sized samples covering source/text, executable/library
patterns, office/document data, database records, VM/zero-heavy storage,
structured binary data, mixed small-file/configuration content and
already-compressed/encrypted-style high-entropy content. It requires:

- the adaptive classifier to select the compressible workload classes and skip
  the high-entropy class;
- deterministic and bit-exact IAC1 round trips;
- filesystem-block savings on at least six representative workload classes;
- at least 30 percent aggregate physical-block savings across the deterministic
  corpus; and
- the IAC1 scratch-memory bound to remain at or below 128 KiB.

The qualification also runs LZ4 over the same corpus and emits per-class and
aggregate size plus encode/decode throughput telemetry. LZ4 remains the
in-tree reference baseline. Zstandard remains useful as an external research
baseline, but it is not a Format 0.17 codec dependency and is deliberately not
required to mount or recover an InfiltratorFS volume.

The native codec is not required to beat a general-purpose codec on every
individual corpus member. Its selection is justified at filesystem level by
the combination of useful block savings, cheap incompressible-data rejection,
bounded memory and decode work, deterministic cross-platform representation,
small kernel implementation surface and bounded mutation amplification.

## Format discipline

For Format 0.17, codec identifier 2 means IAC1 v1 and codec identifier 1 means
the retained LZ4 representation. The codec identifier, stored-byte count and
logical extent length are sufficient to locate and decode each bounded stream.

Future codec research may allocate any unused identifier in the 11-bit Format
0.17 codec namespace or may justify a future development-format revision. It
must not silently change the meaning of an existing IAC1 v1 stream.

Pre-1.0 development may still replace Format 0.17 as a whole, but within a
given accepted format a committed compressed stream must remain deterministic,
recoverable and independently decodable.
