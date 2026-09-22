# InfiltratorFS 0.18.74

## Native Linux sustained-write stall recovery

0.18.74 addresses a system-wide latency failure reproduced by mirroring a live Linux Mint root filesystem onto a populated InfiltratorFS volume with rsync.

The captured workload showed repeated deferred-transaction publications lasting roughly 6–20 seconds, high full-I/O PSI, InfiltratorFS and rsync tasks in uninterruptible sleep, and unrelated interactive display/I2C paths stalling at the same time even though the NVMe device itself was not continuously saturated.

This release changes the native Linux writeback path in four related ways:

- freshly committed CoW dependency ranges are submitted progressively instead of leaving hundreds of MiB of dirty buffer heads to be first issued at the 512 MiB transaction-publication boundary;
- dirty page-cache clusters are snapshotted into bounded staging memory and their folio locks are released before native compression, CoW metadata work, or checkpoint publication can block for a long interval;
- page-cache folio migration is implemented, including preservation of InfiltratorFS's independent pending-CoW accounting marker, removing the Linux compaction warning for a writepages filesystem without migrate_folio; and
- slow transaction publication now reports separate timings for allocation-map construction, dependency draining, the dependency durability flush, checkpoint writes, and the final publication flush.

Progressive dependency submission occurs only after an individual operation is committed into the active deferred transaction. Blocks that are still only volatile allocation reservations are never submitted, preventing rollback/reuse from racing stale in-flight I/O.

The stable-media ordering contract is unchanged: transaction publication still waits for every CoW dependency, performs the dependency durability barrier, writes checkpoint replicas, and performs the final publication barrier before the new generation becomes authoritative.

## Compatibility

- On-disk format remains **0.18**.
- Existing Format 0.18 volumes remain the development target.
- Compression and SHA-256 integrity remain enabled and unchanged in meaning.
- No compatibility promise is introduced before 1.0.

## Validation

The regression source was a real sustained Linux Mint migration rather than a short synthetic throughput test. Exact-source native kernel, metadata, root-volume, resize, userspace and release qualification remain required before immutable publication.
