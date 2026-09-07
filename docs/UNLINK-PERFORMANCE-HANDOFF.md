# InfiltratorFS unlink performance handoff

Status: confirmed live performance defect on the 0.18.45 native Linux VFS path.
Baseline release commit: `99432398db9bae78d638637538c4c9076179dba1` (`v0.18.45`).

## Why this document exists

This is the recovery point before changing the kernel implementation. If the implementation attempt is interrupted or fails, resume from this document and the baseline commit above.

## Reproduction

The real workload is an rsync of a Linux Mint root filesystem from ext4 to a mounted InfiltratorFS volume on the same NVMe device:

```sh
sudo rsync -aHAXx --numeric-ids --delete \
  --exclude='/boot/efi/***' --info=progress2 \
  / /mnt/infiltratorfs-root/
```

The destination volume was scrubbed immediately before the run:

- generation: 6067
- files checked: 14656
- data blocks checked: 21815
- snapshots checked: 0
- checksum errors: 0
- metadata errors: 0
- result: CLEAN

The native module was rebuilt from the final 0.18.45 release source for kernel `7.0.0-31-generic` and reloaded before the test.

## Measured evidence

`iostat -xz 1 nvme1n1` showed the NVMe almost idle while rsync appeared stalled: typically about 0.2-0.6% device utilisation and very low I/O throughput.

The active rsync worker was charged close to one full CPU core. A 10-second syscall profile resolved where that CPU time was being spent:

```text
% time     seconds  usecs/call  calls  syscall
100.00    9.654321      219416     44  unlink
```

That is approximately 219 ms per `unlink()`.

This proves the hot interval is inside the destination filesystem's unlink path, not rsync's userspace hard-link bookkeeping and not NVMe throughput.

## Confirmed hot path in 0.18.45

The native namespace delete/reclaim path eventually calls:

- `infilfs_ns_delete_file_resources()`
- `infilfs_ns_free_unshared_run()`
- `infilfs_ns_other_reference_cover()`

`infilfs_ns_other_reference_cover()` currently snapshots the object index, walks every other live file, reads that file's extents, and searches for overlap with the range being reclaimed.

Therefore reclaiming one extent may require a whole-filesystem ownership scan. Repeating this during a delete-heavy rsync makes unlink cost scale with the live file/extent population and can become effectively quadratic over a bulk cleanup.

This is the previously identified structural weakness: reflink/snapshot correctness is preserved by scan-based shared-block ownership discovery, but ownership lookup is too expensive for real delete-heavy workloads.

## Required fix

Replace repeated whole-filesystem shared-range discovery in the native Linux delete/reclaim path with a volatile kernel shared-extent ownership/refcount index.

Correctness requirements:

1. A range may only be returned to the allocator when its live reference count reaches zero.
2. Reflinks and snapshots must continue preventing premature block reuse.
3. Normal unshared files must take a fast path with no full object-index scan per unlink.
4. The index must be rebuildable from authoritative on-disk metadata after mount/crash; it is an acceleration structure, not new source-of-truth metadata.
5. Mutations that create, replace, clone, snapshot, truncate, or delete extents must keep the volatile index coherent, or conservatively invalidate/rebuild it before reclamation.
6. Allocation/free accounting and crash-consistency semantics must not change.
7. If memory allocation for the acceleration index fails, correctness must be preserved by a safe fallback rather than freeing uncertain blocks.

## Implementation strategy

Preferred first implementation for the 0.18.x format:

- add a mount-private interval/refcount structure keyed by physical block ranges;
- populate it from authoritative live-file extents (and any snapshot roots that participate in ownership) once per mount or lazily on first ownership query;
- split/merge interval records as counts change;
- increment counts when a live reference to a physical range is introduced;
- decrement counts when a reference is retired;
- reclaim only subranges whose resulting count is zero;
- keep the existing scan implementation available as a conservative fallback while the index is unavailable or invalid.

The on-disk format does not need to change for this volatile acceleration structure.

## Qualification required before release

At minimum:

- kernel module builds against the CI and current native kernel headers;
- existing native VFS/reflink/snapshot/crash/recovery tests remain green;
- add a delete/reclaim qualification that creates thousands of files and verifies unlink does not perform O(files) ownership scans per file;
- include a shared-extent case proving deleting one reflink does not free blocks still referenced by another;
- scrub result CLEAN after stress/reclaim;
- repeat the real Mint rsync workload and measure syscall latency/device utilisation again.

Practical success target: bulk unlink should fall from ~219 ms/file by orders of magnitude on an unshared 14k-file volume. Exact throughput is hardware/workload dependent, so CI should assert algorithmic behaviour/counters rather than a fragile wall-clock threshold.

## Recovery / continuation instructions

If work is interrupted:

1. Read this file.
2. Confirm current `main` is descended from baseline `99432398db9bae78d638637538c4c9076179dba1` and from the documentation commit containing this file.
3. Inspect `kernel/infiltratorfs_rw_namespace.inc` around `infilfs_ns_other_reference_cover`, `infilfs_ns_free_unshared_run`, and `infilfs_ns_delete_file_resources`.
4. Do not remove the ownership check merely to gain speed; that would corrupt reflinks/snapshots by freeing shared blocks.
5. Continue with a correct ownership/refcount acceleration structure and qualify it before publishing another release.
