<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Forensic metadata scanning

`infilfs-forensic` is a read-only physical-block scanner for current Format 0.19 media. It does not require the current namespace graph to open and does not modify, repair, mount or replay the target.

```bash
infilfs-forensic volume.img
infilfs-forensic --jsonl /dev/sdb1 > evidence.jsonl
```

The scanner authenticates each recognized current-format record independently using the Format 0.19 validation contract, including symbolic-link, hard-link and snapshot-catalog objects:

- checkpoint structure, geometry-independent fields and CRC64;
- object magic, type/version, identity, payload shape, canonical padding and CRC64;
- directory leaf/branch-page ownership, generation, bounds, canonical padding and CRC64;
- index leaf/branch-page ownership, generation, bounds, canonical padding and CRC64; and
- extent-page ownership, generation, bounds, canonical padding and CRC64;
- allocation branch-page generation, logical index, level, child-count/payload shape and CRC64; and
- allocation leaf-page generation, logical index, valid-bit count/payload shape and CRC64.

Arbitrary data that merely contains a magic string is not reported. A candidate must satisfy its complete block-level validation contract.

## Record state

When a valid checkpoint and allocation tree survive full graph validation, the scanner reconstructs the authoritative allocation bitset and reports:

- `current` for metadata allocated by the selected live generation or a retained snapshot graph, and checkpoint replicas matching the selected generation;
- `stale` for an independently valid older checkpoint replica; and
- `orphaned` for an authenticated object or page that is no longer allocated by the live allocation bitset, commonly a superseded CoW record.

When no trustworthy current checkpoint/allocation-tree pair is available, authenticated raw records are still reported with state `unknown`. This is deliberate: raw discovery remains possible without claiming that a record belongs to an authoritative namespace.

## Relationship to recovery and scrub

Forensic discovery is intentionally separate from normal mount recovery. The normal opener selects from structurally valid committed checkpoint graphs. `fsck.infiltratorfs --scrub` validates the live and retained snapshot graphs. `infilfs-forensic` instead scans physical blocks for independently authentic records, including material that may no longer be reachable from the selected live graph.

A forensic hit therefore proves that a block satisfies the record-level authentication contract; it does not by itself prove that the block belongs to the authoritative current namespace or that its referenced file payload is complete.

## Output contract

The default output is tab-separated and ends with a summary row. `--jsonl` emits one JSON object per discovered record followed by a summary object. Each metadata row includes its physical block, classification, kind, generation, object type/version, object and parent identities, payload size, entry count and bytes used where those fields apply. Allocation pages additionally expose logical index and tree level.

The scanner discovers metadata only. It does not yet reconstruct paths from historical directory/index combinations, recover file payloads, resolve competing historical graphs or write repaired structures.

## Future accidental-format recovery

The existing scanner is one possible foundation, not a required implementation. A redesigned independent recovery engine is acceptable, including a new development format and new recovery metadata. The goal is recovery of complete historical filesystem structure and verified payloads after native recoverable formatting or foreign quick formatting/repartitioning, not merely enumeration of surviving blocks.

Default to read-only raw disk or partition access, preferably against an immutable image; search candidate original geometries and distributed records even if partition tables and primary anchors are lost. Full-device scanning is explicit offline work. Group candidates by volume identity, format and generation; validate graph coherence, ownership, bounds, cycles, aliasing and integrity. A magic value, CRC or highest generation alone is not proof of a committed graph. Reconstruct names, hierarchy, file extents, links, timestamps, security metadata and snapshots only where dependencies survive. Verify logical data digests and encrypted-record authentication where applicable.

Report verified complete objects, partial objects with missing ranges, competing ambiguous candidates and unrecoverable objects. Do not silently substitute zeros or claim unverified content as valid. Export to separate storage by default and report any metadata losses; in-place restoration requires an exclusive target, a validated complete graph, verified independent backup/export, explicit authorisation and crash-safe publication followed by scrub. No universal recovery after overwrite, discard, secure erase or key loss is possible. This is a design requirement, not a current scanner capability.

---

## Proposed Format 0.20 — unified design (revisions 3–4)

**Proposal only; the preceding current-format contract remains applicable to Format 0.19.** The [unified Format 0.20 specification](ON_DISK_FORMAT.md#proposed-infiltratorfs-on-disk-format-020) is the design authority for the proposed persistent model. This section specialises that model; it does not introduce a second on-disk authority or claim implementation.

One Unformatter uses shared record validation, committed semantic/location membership, ownership reconstruction and per-allocation integrity for both native reversal and foreign-format damage. Native format retains the old roots in the same allocator; offline foreign recovery discovers the ordinary protected canonical records even without primary indexes or a partition table. A location mapping is essential information, not inferable from an untagged data block. Rebuild indexes and physical ownership only from a coherent committed record set; prepared/stale records with valid CRC are salvage unless membership is established. Resolve surviving committed proof nodes by identity and digest; missing all copies of a required proof node limits committed reconstruction. Report exact metadata and payload completeness separately, including missing ranges and historical/ambiguous states. Read-only discovery/export is default. In-place foreign reconstruction requires complete evidence, independent verified backup/export, safe staging, exclusive authority and scrub; healthy native undo uses an authorised root switch and retains displaced writes by default. No guaranteed recovery after complete overwrite, device-wide discard or key loss.

### Revision 4 — committed membership and independent reconstruction
The ordinary protected Merkle tree is the membership proof, not a second recovery namespace. The scan index locates alternative copies by expected immutable identity/digest. If all copies of a required proof node are missing, remaining records may support salvage but must not be silently promoted to the exact committed graph. Derived indexes are rebuildable from proven essential records; allocation location maps are essential. The canonical export manifest verifies semantics and payload completeness independently of tree shape. Section 20.17 defines the common recovery procedure and native root-selection path. No new scanner implementation is implied.
