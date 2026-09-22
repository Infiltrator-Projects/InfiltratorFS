# 2026 Filesystem Research Cross-Check

This note records the September 2026 forensic cross-check of the current
InfiltratorFS Format 0.18 Linux implementation against the failure modes
described in the filesystem papers and advisories reviewed during development.
It records code invariants and remediation, not a claim that InfiltratorFS
shares another filesystem's implementation.

## Confirmed issues and remediation

### Stable-media publication ordering

The native transaction path used to wait for transaction dependency I/O, write
all generation-N+1 checkpoint replicas, and issue its only block-device cache
flush afterwards. I/O completion alone is not a stable-media ordering boundary
for a device with a volatile write-back cache. A power loss could therefore
make a new checkpoint durable before every dependency it references.

The transaction contract is now:

1. build the replacement allocation graph;
2. submit and wait for this transaction's CoW dependency writes;
3. issue a block-device cache flush so every dependency is stable;
4. synchronously write all checkpoint replicas; and
5. issue the publication cache flush before installing the new volatile live
   allocation state.

The source policy guard requires two cache flushes and verifies that the first
is between dependency completion and the first checkpoint write.

### Reflink checksum-chain scale

Format 0.18 binds checksum objects to the owning file object. Reflink therefore
copies checksum metadata even though file-data extents are shared. The previous
kernel implementation first materialized the complete checksum chain in memory
and also stopped after 1,048,576 checksum objects. With 123 4 KiB block digests
per checksum object, that imposed an implementation ceiling of about 492 GiB on
a dense fully checksummed reflink.

Checksum cloning is now streaming and bounded-memory. Clone object IDs are
generated one node ahead, checksum objects are staged as they are visited, and
object-index additions are published in fixed-size batches. Cycle/corruption
protection is derived from the source file's logical block count and the format
checksum density rather than an arbitrary one-million-node limit.

This removes the 492 GiB implementation ceiling. The Format 0.18 owner binding
still means checksum metadata work is O(number of checksum objects); removing
that cost entirely would require a persistent-format change rather than a safe
kernel-only optimization.

### Reflink ownership-index invalidation

The shared-range accelerator is rebuildable volatile state and is not the
source of truth. Previously every non-inline reflink invalidated it. A following
truncate, hole punch, overwrite, defrag or final reclamation could then rebuild
ownership by walking all live file extents. Repeated clone/small-truncate
workloads therefore triggered unexpectedly large global metadata discovery.

When the accelerator is valid, reflink now increments reference multiplicity
only across the cloned source physical ranges. Private ranges become refcount
2; already-shared intervals increment in place. If the accelerator has never
been built since mount, it remains invalid and the existing exact fallback is
unchanged. Allocation failure while updating the accelerator invalidates it
conservatively rather than risking a false negative.

### Power-loss qualification host caching

The real root-boot qualification already kills QEMU while the InfiltratorFS
root is actively writing, but QEMU's default host write-back cache can preserve
guest writes in the host page cache after the VM process is killed. The raw
root disk is now opened with `cache=none`, so the test no longer relies on host
page-cache survival to represent the guest disk across the forced stop.

This strengthens the existing test but is not equivalent to a hardware power
cut with programmable persistence reordering. Block-level fault injection
remains the stronger future evidence class for exhaustive ordering tests.

## Failure modes not reproduced

The reviewed stale-mapping/direct-I/O race was not found in the current native
mutation paths. Extent collection and replacement remain under the serialized
native topology mutation region; code does not deliberately drop that
protection and then reuse a previously resolved physical mapping.

The reviewed premature "not shared" state failure was also not found. A valid
InfiltratorFS shared-range accelerator is conservative: ordinary CoW mutation
may leave false positives, but must not create false negatives. Reflink is the
operation that creates an additional live owner, and it now updates the valid
accelerator directly.

The reviewed reclaim/writeback circular wait was not reproduced. InfiltratorFS
reservations either succeed or return an error rather than sleeping in
filesystem reclaim waiting for dirty folios whose writeback needs the same
reservation. Folios are still held across synchronous native writeback
submission, so latency under writer contention remains a performance
measurement target, not a confirmed deadlock.

## Performance observations retained for profiling

Buffered writeback can still copy page-cache data into a contiguous private
staging buffer before compression and integrity preparation. That is deliberate
today because the codec operates on bounded contiguous clusters. Likewise,
writeback may keep folios locked while waiting for native CoW submission.
Neither was changed in this audit because the paper-derived concern alone does
not establish a correctness defect, and changing either path without workload
profiling would risk mmap/writeback and durability semantics.

The appropriate gate for future changes to those paths is the existing mounted
write, mmap, fsync, reflink, truncate, hole-punch, crash-recovery and scrub
qualification matrix plus explicit CPU/latency measurements.
