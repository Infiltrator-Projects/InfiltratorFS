<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# IAC1 / Native Write Performance Handoff

## Scope

This document records the first real-machine performance failure that remained after the native unlink ownership fix in InfiltratorFS 0.18.46, together with the implementation direction that should be preserved if work is interrupted.

The evidence below was collected against the exact released 0.18.46 source commit:

`ebe283e2b6251ad7e72ba56ee3204c5f30b8c215`

The test workload was a real Linux Mint root migration with:

```sh
rsync -aHAXx --numeric-ids --delete --exclude='/boot/efi/***' / /mnt/infiltratorfs-root/
```

The source and target were partitions on the same 2 TB Kioxia NVMe device. The target was Format 0.18, mounted native Linux read-write with `compress=auto`.

This is a performance handoff, not a declaration that the IAC1 format is defective. IAC1 v1 remains the Format 0.18 native codec and existing IAC1 streams remain authoritative and decodable.

## What the live test proved

0.18.46 removed the previous pathological unlink behaviour. The migration then made steady progress, but only at roughly 20-31 MiB/s on a modern NVMe system.

The storage device was not saturated. During the workload the NVMe was commonly only about 6-14 percent busy, with a small queue and sub-millisecond read latency, while the rsync writer was close to one full CPU.

A 10 second `strace -c` sample of the live rsync writer reported:

```text
98.90%   8.709318 s   8941 us/call    974 calls   write
 0.66%   0.058008 s      3 us/call  17066 calls   read
 0.44%   0.038494 s      2 us/call  17066 calls   pselect6
```

The average destination `write()` was therefore taking about 8.9 ms and almost all syscall time was being spent in writes.

A 10 second `perf record -g -p <rsync-pid>` sample then identified the dominant CPU consumers inside the InfiltratorFS kernel path:

```text
40.73%  infs_iac1_compress_mode
23.64%  infilfs_rw_sha256_transform
10.38%  infilfs_crc64_block_valid
```

The captured stacks placed IAC1 directly under:

```text
write()
  -> infilfs_file_write_iter
    -> infilfs_file_write_iter_common
      -> infilfs_native_extent_write_iter_mode
        -> infilfs_native_append_chunk_locked
          -> infs_iac1_compress_mode
```

This is the central diagnosis: the application thread is currently doing expensive compression and integrity work synchronously in the native write path while the NVMe has substantial unused capacity.

## IAC1 is not being abandoned

The live profile does not invalidate the reasons IAC1 was introduced.

IAC1 v1 still has useful filesystem properties:

- independently decodable bounded streams of at most 64 filesystem blocks (256 KiB);
- deterministic, bounded-memory operation;
- identity, byte-delta and four-byte XOR predictor modes;
- a bounded 64 KiB backward match window;
- explicit fill runs for zero-heavy data;
- no global dictionary or hidden mutable persistent state;
- portable-core and native-kernel decode compatibility;
- corruption rejection through strict stream validation; and
- adaptive fallback to uncompressed storage when a stream does not save at least one filesystem block.

The existing qualification suite also requires IAC1 to save filesystem blocks on representative workload classes and at least 30 percent aggregate physical space across its deterministic corpus. Those are meaningful properties and must remain qualified.

What the real Mint migration exposed is that the current encoder and its integration into the Linux write path are too CPU-expensive. That is an implementation and scheduling problem. It is not, by itself, evidence that the on-disk IAC1 v1 representation should be discarded.

## Source-level causes confirmed in 0.18.46

### 1. The compressor runs in the caller's write path

Normal native file operations use `infilfs_file_write_iter`, not the generic buffered file writer. Eligible sequential EOF writes reach `infilfs_native_append_chunk_locked`, where compression is attempted synchronously.

This means a single-threaded writer such as rsync naturally drives one compression context at a time. Other CPUs cannot help merely because they are idle.

### 2. The expensive work is too close to global write serialization

The native data path still uses the mount-wide write transaction lock for topology/publication work. Expensive data preparation must not remain inside, or be made dependent on, that serialized critical section.

The required architectural split is:

```text
parallel data preparation
    -> compression
    -> logical digest preparation
    -> stored representation preparation

short serialized publication
    -> allocation consumption
    -> extent/index CoW
    -> checksum/object metadata updates
    -> transaction publication bookkeeping
```

### 3. IAC1 clears a large scratch table for every full mode pass

`struct infs_iac1_scratch` contains two 16,384-entry 32-bit arrays (`latest` and `previous`), for a fixed 128 KiB scratch structure.

`infs_iac1_compress_mode()` clears the full scratch state before encoding. A 256 KiB compression unit can therefore touch 128 KiB of scratch simply to reset the match finder before doing useful compression work.

If two full mode passes are performed, the scratch reset cost is paid twice.

Preferred fix: retain the 128 KiB bound but replace full-table clearing with generation-tagged entries. Because a 256 KiB stream needs only 18 bits to encode an inserted position, the existing 32-bit slots have room for a bounded epoch/tag. Clear the table only on epoch wrap. This must be implemented carefully and qualified for wraparound, determinism and stale-entry rejection.

### 4. Predictor work is repeated

The current filesystem compression callers compute `infs_iac1_predictor_mode()` before allocating an alternate candidate buffer, then call `infs_iac1_compress()`, which computes predictor mode again internally.

For selected inputs, `infs_iac1_compress()` also performs an identity-mode full compression and can then perform a second full compression in the selected predictor mode before retaining the smaller result.

The default hot path should not repeatedly scan and encode the same 256 KiB input.

Preferred fix:

- make the cheap sampler estimate both compressibility and likely predictor;
- pass the selected predictor into the encoder instead of recomputing it;
- perform one full encode by default;
- allow a second full candidate pass only for explicitly ambiguous cases where the sample cannot confidently choose a mode; and
- preserve deterministic output for a fixed policy/implementation.

Do not change the IAC1 v1 decoder contract merely to obtain this optimisation. Encoder policy may improve while all valid committed v1 streams remain decodable.

### 5. SHA-256 is a measured hotspot

The native kernel path currently uses the in-tree scalar SHA-256 transform. In the live profile it consumed about 23.6 percent of sampled cycles.

Preferred Linux fix: use the kernel Crypto API (`shash`/`sha256`) in the native kernel adapter so the kernel can select the best registered SHA-256 implementation for the CPU. Keep the portable/offline implementation available for userspace tools and cross-platform recovery. Digest bytes must remain identical.

This is an implementation substitution, not an integrity-policy reduction.

### 6. CRC64 validation is being repeated in the write path

`infilfs_crc64_block_valid` accounted for about 10.4 percent of the live sample, much of it below metadata/extent validation during writes.

Integrity validation must remain fail-closed, but immutable or generation-stable metadata that has already been validated should not necessarily be re-CRC'd every time it is revisited in the same known generation.

Preferred fix: introduce a bounded generation-aware validation cache, or an equivalent proof of freshness, and invalidate it whenever the relevant physical block can be replaced/reused or the authoritative mapping changes. Do not simply remove CRC checks.

## Multicore design decision

Multicore compression is required, but the preferred design is not to split one 256 KiB IAC1 stream internally across many threads.

A single stream is deliberately bounded and independently decodable. Internally parallelising a small individual stream adds scheduling overhead, complicates deterministic parsing/encoding and risks changing the format for little gain.

The better unit of parallelism is **independent compression clusters**.

The desired pipeline is:

```text
application buffered write
        |
        v
page cache / dirty folios
        |
        v
coalesce contiguous dirty data into bounded IAC1 clusters
        |
        +--> worker A: cluster 0 -> classify -> IAC1 -> digest
        +--> worker B: cluster 1 -> classify -> IAC1 -> digest
        +--> worker C: cluster 2 -> classify -> IAC1 -> digest
        +--> worker D: cluster 3 -> classify -> IAC1 -> digest
        |        ... bounded by CPU and memory policy ...
        v
collect prepared clusters in logical order
        |
        v
brief allocation / CoW / metadata publication phase
```

This allows even a single-threaded application to benefit from several CPUs because the application thread is no longer required to finish compression before the next logical cluster can become dirty.

### Use Linux buffered writeback as the staging mechanism

InfiltratorFS already has page-cache address-space operations for reads, mmap dirties and writeback. The current `writepages` implementation, however, walks dirty folios and writes them back page-sized through the same native extent writer. Normal `write(2)` still uses the custom synchronous native writer directly.

The long-term native Linux path should converge on normal buffered-write semantics:

- ordinary writes copy into page cache and dirty folios;
- writeback gathers contiguous dirty folios into 256 KiB compression clusters where appropriate;
- compression/digest preparation happens outside the global metadata lock;
- independent clusters are dispatched to a dedicated bounded worker pool;
- writeback/publication observes Linux dirty/writeback/error semantics;
- `fsync`, `sync`, `O_SYNC`, `O_DSYNC` and `RWF_SYNC`/`RWF_DSYNC` drain the required range and publish it with the required durability; and
- mmap and write(2) remain coherent through the same page-cache state.

Do **not** invent a private asynchronous queue that can return from write(2) while data exists only in an InfiltratorFS-private buffer with no VFS dirty/writeback accounting. Use the Linux page cache/writeback contract as the authoritative staging mechanism.

### Worker-pool requirements

Compression is CPU-intensive work. The implementation should use a dedicated bounded kernel workqueue rather than `system_wq` for compression jobs.

The worker design must:

- permit true execution on multiple CPUs;
- bound in-flight clusters and total staging memory;
- apply backpressure instead of allowing unbounded dirty/compression queues;
- support cancellation/drain during unmount and error recovery;
- preserve ordered logical publication even when compression finishes out of order;
- propagate per-cluster errors to mapping/writeback error state;
- avoid waiting for compression workers while holding the filesystem metadata write lock; and
- use memory-reclaim-safe workqueue semantics if the queue is reachable from writeback/reclaim paths.

The number of active workers should be dynamically bounded by online CPUs and memory pressure rather than hard-coded to one or to every CPU unconditionally.

## Options considered and rejected

### Disable compression by default

Rejected as a product direction. `compress=off` is useful as an A/B diagnostic, but it gives up a core InfiltratorFS feature rather than fixing it.

### Replace IAC1 with another codec

Rejected. The evidence does not show that IAC1's on-disk design is invalid. It
shows that IAC1's current encoder and synchronous kernel integration are
expensive.

A future codec change remains possible if measurement after optimisation shows IAC1 cannot meet the required filesystem-level ratio/throughput goals. That decision should be evidence-driven, not made from the current profile alone.

### Run the current IAC1 implementation unchanged on many CPUs

Rejected as incomplete. It would raise throughput by spending more total CPU on known waste: repeated scratch clears, repeated predictor scans and potentially dual full encodes. Improve the encoder and parallelise the pipeline.

### Parallelise identity and predictor candidates only

Rejected as the main strategy. It would use two CPUs to preserve the current double-work policy but would still perform unnecessary duplicate encoding. It may be useful only as a temporary diagnostic.

### Increase the compression-cluster size as the primary fix

Rejected. Larger clusters change latency, memory use, mutation amplification and format/recovery tradeoffs and do not solve the single-threaded synchronous write architecture.

### Remove SHA-256 or CRC verification

Rejected. Integrity is part of the filesystem contract. Use faster implementations and eliminate redundant validation, not protection.

## Implementation order

The recommended order is deliberately staged so each improvement can be measured and correctness can be isolated.

1. **Preserve this live regression as a permanent benchmark.** Add mounted `compress=auto` versus `compress=off` write telemetry, write-call latency and device-utilisation evidence where practical. Keep CI thresholds relative/generous enough to avoid runner noise.
2. **Optimise IAC1 single-cluster encode cost.** Generation-tagged scratch entries, no redundant predictor scan, one full encode by default, bounded ambiguous fallback.
3. **Replace native scalar SHA-256 with the kernel Crypto API.** Keep portable SHA for offline/userspace paths.
4. **Reduce redundant metadata CRC64 work** with generation-aware validated-state caching while retaining fail-closed checks.
5. **Move ordinary Linux writes onto proper buffered page-cache semantics** and make clustered writeback the authoritative compression entry point.
6. **Add bounded multicore compression/digest workers** for independent dirty clusters, outside metadata serialisation.
7. **Shorten final metadata publication critical sections** so parallel preparation is not undone by a long global lock hold.
8. **Re-profile the same Mint migration.** Only then decide whether IAC1 itself needs deeper algorithmic change or a future codec should replace it for new extents.

Steps 2-4 can improve 0.18-compatible implementation behaviour without changing the meaning of existing IAC1 v1 streams. Step 5 is a larger Linux I/O architecture change and must be qualified especially heavily for fsync/durability, mmap, append, reflink/CoW, truncate, hole punch and crash recovery.

## Required qualification before calling the performance work complete

Correctness must remain at least as strong as 0.18.46. Add or preserve tests for:

- deterministic IAC1 encode for repeated runs under the same encoder policy;
- decode compatibility with IAC1 v1 streams produced before optimisation;
- predictor modes, malformed-stream rejection and roundtrip correctness;
- current representative-corpus physical savings floor;
- incompressible-data rejection;
- partial overwrite, truncate, hole punch, fallocate, reflink and snapshot semantics;
- mmap/write(2) coherence;
- fsync, sync, O_SYNC/O_DSYNC and RWF_SYNC/RWF_DSYNC durability;
- power-loss/crash recovery with dirty writeback and completed-but-unpublished compression jobs;
- worker cancellation/drain on unmount;
- memory-pressure/reclaim progress without workqueue deadlock;
- checksum equality between portable SHA-256 and native Crypto-API SHA-256;
- generation-cache invalidation for CRC metadata validation;
- bounded memory with many concurrent writers; and
- no regression of the 1023-byte filename, quota, ACL/xattr, reflink, snapshot or resize paths.

Performance qualification should separately measure:

- IAC1 encode/decode MiB/s on the deterministic 256 KiB corpus;
- `compress=auto` versus `compress=off` mounted sequential-write throughput;
- small-file and mixed-tree throughput;
- average and tail write syscall latency;
- CPU utilisation and number of CPUs used during compressible writes;
- NVMe utilisation/queue depth during the workload;
- physical block savings; and
- aggregate CPU time per logical GiB written.

A successful result is not merely 'all CPUs are busy'. The desired result is substantially lower CPU work per byte **and** useful parallel scaling until storage, memory bandwidth or a later filesystem stage becomes the real bottleneck.

## Current conclusion

IAC1 should be treated as **promising but not performance-finished**.

Its Format 0.18 stream design, boundedness, corruption handling and measured space-saving behaviour remain valuable. The live Mint migration has exposed a serious encoder/write-path efficiency hole that the synthetic qualification did not gate strongly enough.

The preferred solution is therefore both:

1. make IAC1 and integrity preparation much cheaper per cluster; and
2. move compression out of the caller's synchronous serialized write path into bounded multicore buffered writeback.

Do not throw away IAC1 solely because of the 0.18.46 profile. Fix the measured implementation costs first, re-profile the exact real workload, and only then make any decision about replacing the codec.
