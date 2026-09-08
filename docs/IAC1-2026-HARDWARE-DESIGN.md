<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# IAC1 2026 Hardware Design Principle

## Purpose

This document records a permanent design rule for InfiltratorFS compression:

**IAC1 is intended to be a filesystem-native compressor designed for contemporary multicore, NVMe-era hardware, not an old single-threaded compression model transplanted into a new filesystem.**

This principle complements `docs/COMPRESSION.md` and `docs/IAC1-WRITE-PERFORMANCE-HANDOFF.md`.

The Format 0.18 IAC1 v1 stream is not being discarded. The current performance work is about bringing the encoder and Linux write pipeline up to the hardware assumptions InfiltratorFS was meant to target.

## Separation of format and implementation

The on-disk format and the encoder implementation have different jobs.

The persistent IAC1 stream should remain conservative, deterministic, bounded and recoverable:

- independently decodable bounded streams;
- deterministic representation for a defined encoder policy;
- strict corruption rejection;
- bounded memory requirements;
- no hidden persistent global dictionary;
- portable decoding across Linux, Windows and offline/recovery tools; and
- no requirement for special CPU features merely to read committed data.

The encoder is allowed, and expected, to be much more aggressive internally:

- multicore cluster preparation;
- per-CPU or worker-local scratch state;
- SIMD/vector acceleration where it improves measured throughput;
- CPU-feature dispatch where useful;
- hardware-accelerated hashing through the platform's supported crypto facilities;
- asynchronous/buffered writeback;
- batched allocation and metadata preparation;
- memory-bandwidth-aware algorithms; and
- dynamic concurrency based on online CPUs, memory pressure and storage capability.

A modern encoder may therefore be very different internally while still emitting fully valid IAC1 v1 streams that the small portable decoder can read.

## 2026 hardware assumptions

The implementation should be designed around the capabilities normally available on current systems rather than around historical low-core-count or SATA-era constraints.

The design should assume that many target systems have:

- multiple CPU cores, often with simultaneous multithreading or heterogeneous cores;
- fast memory and large caches;
- wide SIMD/vector instruction sets;
- hardware acceleration for common cryptographic primitives;
- NVMe storage capable of high queue depth and high parallel throughput; and
- enough RAM to trade a bounded amount of memory for substantially better storage throughput.

These are opportunities, not mandatory mount requirements. InfiltratorFS must still function correctly on a machine without a particular acceleration feature. Fast paths must have portable fallbacks.

## Core performance rule

A single application writer must not force the whole compression path to behave as a single-core pipeline when independent compression work is available.

The preferred unit of parallelism is the independent bounded compression cluster, not pieces of one IAC1 stream.

For large or sustained writes, the desired architecture is:

```text
application write
      |
      v
Linux page cache / dirty folios
      |
      v
coalesce contiguous dirty data into bounded IAC1 clusters
      |
      +--> worker 0: cluster A -> classify -> encode -> digest
      +--> worker 1: cluster B -> classify -> encode -> digest
      +--> worker 2: cluster C -> classify -> encode -> digest
      +--> worker 3: cluster D -> classify -> encode -> digest
      |        ... bounded dynamically ...
      v
collect prepared clusters in logical order
      |
      v
short CoW / allocation / metadata publication phase
      |
      v
NVMe
```

This lets a single-threaded program such as rsync benefit from multiple CPUs without changing the IAC1 stream format.

Parallelism must be bounded. The goal is not to keep every CPU busy at any cost. The goal is to reduce wall-clock latency and CPU work per byte while keeping the storage device fed.

## IAC1 encoder direction

The current IAC1 encoder should be improved as an IAC1 encoder, not replaced merely because its first native Linux implementation is too CPU-expensive.

The 0.18.46 Mint migration profile showed the following dominant sampled costs:

```text
40.73%  infs_iac1_compress_mode
23.64%  infilfs_rw_sha256_transform
10.38%  infilfs_crc64_block_valid
```

The corresponding `strace -c` sample showed about 8.9 ms per destination `write()` and about 98.9 percent of syscall time inside `write()`, while the NVMe device had large unused capacity.

That evidence requires both algorithmic and architectural work.

### Reduce work per cluster

The encoder should:

- avoid clearing the full 128 KiB match-finder scratch structure on every pass where a generation/tagged representation can safely reuse it;
- avoid redundant predictor scans;
- normally perform one full encode rather than an identity encode followed by a second full predictor encode;
- use cheap sampling to reject incompressible input before expensive work;
- allow early termination when the encoder can no longer save enough filesystem blocks to justify compression;
- preserve bounded memory use; and
- retain deterministic decoding and the existing IAC1 v1 stream contract.

### Use modern CPU capabilities where measurement justifies them

The encoder should be open to SIMD/vector acceleration for operations such as:

- match comparison;
- repeated-byte/fill detection;
- predictor transforms;
- sampling/classification; and
- bulk copies.

No particular instruction set should become part of the on-disk format. CPU-specific paths must produce valid streams and have a portable fallback.

### Per-worker state

Compression workers should use worker-local or otherwise contention-free scratch state so parallel encodes do not serialize on shared match-finder memory.

The implementation should avoid false sharing and unnecessary allocator churn. Reusable bounded buffers are preferred over repeated allocation/free cycles on the hot path.

## Integrity is part of the 2026 design too

Modernisation must not trade away integrity.

The SHA-256 result from the live profile is a reason to use the operating system's accelerated crypto implementation, not a reason to weaken the checksum policy.

For native Linux:

- use the kernel Crypto API for SHA-256 so the kernel can select an accelerated implementation for the active CPU;
- keep the portable scalar implementation for userspace/offline/recovery use;
- require bit-identical digest results across implementations; and
- allow digest preparation to run in parallel with independent compression clusters where ordering permits.

Similarly, the CRC64 hotspot should be addressed with safe validated-state caching or equivalent generation-aware proof of freshness. Repeated validation of unchanged metadata should be reduced, but corruption checks must remain fail-closed.

## NVMe-aware pipeline rule

The storage device should not sit mostly idle while one CPU prepares each write serially.

The filesystem should maintain enough bounded prepared work to keep modern NVMe devices usefully occupied, subject to memory pressure and durability requirements.

This does not mean forcing maximum queue depth at all times. Small synchronous writes, latency-sensitive operations, `fsync`, `O_SYNC`/`O_DSYNC`, reclaim and low-memory conditions may require lower concurrency or immediate draining.

The scheduler should adapt rather than assume that one queue depth or one worker count is correct everywhere.

## Dynamic scaling rather than hard-coded thread counts

Do not encode assumptions such as 'four compression threads' into the design.

Worker concurrency should be bounded dynamically by factors including:

- online CPU count and available CPU capacity;
- memory pressure;
- number and size of in-flight dirty clusters;
- whether the workload is actually compressible;
- storage latency and observed queue utilisation; and
- foreground latency/durability constraints.

A 4-core machine and a 64-core workstation should both behave sensibly. More CPUs should be useful when independent work exists, but concurrency should stop increasing when another resource becomes the real bottleneck.

## Decoder policy

The decoder is deliberately not required to mirror the encoder's complexity.

The decoder should remain:

- small;
- deterministic;
- bounded;
- easy to audit;
- suitable for kernel, portable userspace and recovery implementations; and
- independent of optional SIMD or hardware acceleration for correctness.

Optimised decoders are welcome, but the simple portable decoder remains the compatibility and recovery floor.

This asymmetric philosophy is intentional: **aggressive modern encoder, conservative universal decoder, stable recoverable stream.**

## Relationship to LZ4 and other codecs

LZ4 remains useful as an in-tree reference and interoperability/development codec. It should continue to provide a benchmark baseline for ratio, encode throughput and decode throughput.

It is not the design target for InfiltratorFS automatic compression. The purpose of IAC1 is not to recreate an older general-purpose codec under a new name. IAC1 should justify itself at filesystem level by combining:

- useful physical block savings;
- low CPU work per logical byte;
- multicore scaling;
- bounded memory;
- fast and safe decode;
- low mutation amplification;
- deterministic recovery semantics; and
- good integration with CoW, snapshots, reflinks and the Linux page cache.

If, after the encoder and pipeline are properly modernised and re-profiled, IAC1 cannot reach acceptable filesystem-level throughput/ratio efficiency, then a future codec for new extents can be considered. That decision must be based on measured results after the known implementation bottlenecks are removed, not on the 0.18.46 profile alone.

## Qualification expectations

Compression qualification must evolve from 'correct and space-saving' to 'correct, space-saving and appropriate for current hardware'.

Permanent tests/benchmarks should track at least:

- IAC1 encode and decode MiB/s per workload class;
- physical block savings per workload class and in aggregate;
- CPU cycles or CPU time per logical GiB;
- single-worker versus multicore scaling;
- number of CPUs actually used for sustained compressible writes;
- average and tail write latency;
- `compress=auto` versus `compress=off` mounted throughput;
- small-file and mixed-tree behaviour;
- NVMe utilisation and queue depth during sustained writes;
- memory consumed by in-flight compression work;
- behaviour under memory pressure;
- `fsync`, sync-write and crash/power-loss semantics;
- deterministic decode compatibility with previously committed IAC1 v1 streams; and
- equality of portable and accelerated integrity digests.

A performance change is successful only if it improves useful work. 'Uses more cores' by itself is not success. The desired outcome is substantially higher throughput and/or lower latency with lower or acceptably bounded CPU cost per byte, while preserving space savings and correctness.

## Permanent design statement

The intended end state is:

**a conservative, recoverable filesystem-native IAC1 stream format backed by a 2026-class implementation that uses multicore CPUs, SIMD/vector acceleration where useful, hardware-assisted integrity primitives, buffered asynchronous writeback, bounded memory and NVMe parallelism without sacrificing deterministic recovery or portable decoding.**

This is the direction to preserve unless later measurement demonstrates a materially better filesystem-level architecture.
