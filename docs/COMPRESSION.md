<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Compression Design

## Purpose

Compression in InfiltratorFS is not intended to mean "pick a conventional codec and make it the filesystem default". The long-term goal is an InfiltratorFS-native, lossless, adaptive compression system designed specifically for a greenfield general-purpose filesystem using current compression research, current processor capabilities and filesystem workload knowledge.

Pre-1.0 development does not preserve development-format compatibility. A convenient prototype choice must not become permanent merely because code already exists.

## Status of LZ4

The current tree contains working LZ4 per-extent machinery and assigns LZ4 a codec identifier. That work is useful and should be retained as a development/reference codec, interoperability test target and possible explicitly selected fast codec.

LZ4 is **not the intended automatic/default InfiltratorFS compression policy** and its current presence must not be interpreted as the final codec decision.

The final default is expected to be an InfiltratorFS-native codec or codec family. Its on-disk identifier, framing and parameters are deliberately not frozen until the design and qualification work below is complete.

## Required properties of the native codec

The native compressor must be:

- strictly lossless and bit-exact across every supported operating-system adapter;
- deterministic, versioned and fully specified by the on-disk format;
- bounded in memory, stack use and worst-case decode work so it is suitable for kernel and low-level filesystem use;
- independently decodable in bounded extents/clusters so random reads do not require decoding an entire file or a mutable global stream;
- parallel-friendly across independent extents and, where practical, within larger compression units;
- corruption-contained so damage to one compressed unit cannot make unrelated extents undecodable;
- compatible with copy-on-write, reflinks, snapshots, scrub, checksums, truncate, hole punching and partial overwrite;
- able to reject incompressible input cheaply and store it uncompressed rather than wasting space or CPU;
- free of hidden mutable dictionaries or machine-specific state needed to recover persistent data; any dictionary, model or transform state required for decoding must be explicit, immutable/versioned and recoverable from the volume itself;
- codec-agile so later research can add or replace codecs without redefining file identity or the rest of the filesystem.

SHA-256 file-data integrity continues to describe the logical uncompressed bytes. Compression is a storage representation beneath that integrity contract, not a replacement for it.

## Research direction

The native design must start from the compression problem rather than from an existing codec API. Candidate techniques should be measured and combined only when they improve the filesystem Pareto frontier. Research areas include, but are not limited to:

- modern Lempel-Ziv-family match finding and parsing;
- finite-state and asymmetric numeral-system entropy coding such as rANS/tANS/FSE-class techniques;
- arithmetic/range coding where its ratio justifies its latency and implementation cost;
- byte/context modelling, delta/XOR prediction and other reversible predictors for structured binary data;
- SIMD/vector-friendly matching, transforms and decode paths;
- adaptive selection between coding modes from cheap per-extent statistics;
- modern learned or model-assisted lossless techniques only where they can be made deterministic, compact, bounded, portable and practical for kernel/filesystem deployment.

No technique is included merely because it is fashionable or historically common. The design may use several modes inside one native codec family when different data classes have materially different optimal representations.

## Filesystem-specific policy

Compression decisions are per extent or bounded compression unit, not a permanent per-filesystem assumption. The compressor should use inexpensive statistics to decide whether to attempt a mode and should fall back to uncompressed storage when expected savings do not justify CPU, latency or extra write amplification.

The design may use workload information available to the allocator/writer, but persistent decoding must never depend on volatile workload classification.

Random access and mutation matter as much as ratio. A small overwrite should require recompressing only a bounded unit. The unit size is a design parameter to be justified by measurements rather than inherited from an existing filesystem or codec.

Already-compressed or high-entropy content must be detected quickly enough that JPEG/AVIF/HEIF, video, archives, encrypted data and similar payloads do not suffer repeated expensive compression attempts.

## What "better" means

The native codec is not judged by compression ratio alone. Qualification must measure at least:

- compressed size;
- compression throughput;
- decompression throughput;
- median and tail latency;
- CPU time and memory use;
- random-read amplification;
- partial-write/recompression amplification;
- parallel scaling;
- behaviour on incompressible data; and
- corruption detection/containment and cross-platform deterministic decoding.

The benchmark corpus must include source/text, executables and libraries, office/document data, databases and VM-style data, structured binary files, sparse/zero-heavy data, mixed small-file trees and already-compressed/encrypted media.

LZ4 and Zstandard should be retained as reference baselines during development. The native codec becomes the automatic default only after it demonstrates a worthwhile filesystem-level Pareto improvement or a clearly justified balance of ratio, latency and resource use across representative workloads. A roadmap checkbox is not complete merely because encoding and decoding code exists.

## Codec and format discipline

Compression metadata must identify the codec and any version/parameters needed to decode an extent. The format must reserve a clean path for additional codecs.

Temporary codecs, temporary cluster sizes and experimental heuristics must be labelled as such in code and documentation. They must not silently become long-term format contracts.

This rule applies more broadly to InfiltratorFS: an implementation convenience is not a design decision.
