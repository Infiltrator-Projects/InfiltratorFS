<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Forensic metadata scanning

`infilfs-forensic` is a read-only physical-block scanner introduced in implementation 0.11.0 and carried forward into current Format 0.14 media. It does not require the current namespace graph to open and does not modify, repair, mount or replay the target.

```bash
infilfs-forensic volume.img
infilfs-forensic --jsonl /dev/sdb1 > evidence.jsonl
```

The scanner authenticates each recognized current-format record independently using the validation rules defined by Format 0.14, including symbolic-link, hard-link and snapshot-catalog objects:

- checkpoint structure, geometry-independent fields and CRC64;
- object magic, type/version, identity, payload shape, canonical padding and CRC64;
- directory leaf/branch-page ownership, generation, bounds, canonical padding and CRC64;
- index leaf/branch-page ownership, generation, bounds, canonical padding and CRC64; and
- extent-page ownership, generation, bounds, canonical padding and CRC64.

Arbitrary data that merely contains a magic string is not reported. A candidate must satisfy its complete block-level validation contract.

## Record state

When a valid checkpoint and readable bitmap survive, the scanner reports:

- `current` for metadata allocated by the selected live generation or a retained snapshot graph, and checkpoint replicas matching the selected generation;
- `stale` for an independently valid older checkpoint replica; and
- `orphaned` for an authenticated object or page that is no longer allocated by the live bitmap, commonly a superseded CoW record.

When no trustworthy current checkpoint/bitmap pair is available, authenticated raw records are still reported with state `unknown`. This is deliberate: raw discovery remains possible without claiming that a record belongs to an authoritative namespace.

## Relationship to recovery and scrub

Forensic discovery is intentionally separate from normal mount recovery. The normal opener selects from structurally valid committed checkpoint graphs. `infilfs-scrub` validates the live and retained snapshot graphs. `infilfs-forensic` instead scans physical blocks for independently authentic records, including material that may no longer be reachable from the selected live graph.

A forensic hit therefore proves that a block satisfies the record-level authentication contract; it does not by itself prove that the block belongs to the authoritative current namespace or that its referenced file payload is complete.

## Output contract

The default output is tab-separated and ends with a summary row. `--jsonl` emits one JSON object per discovered record followed by a summary object. Each metadata row includes its physical block, classification, kind, generation, object type/version, object and parent identities, payload size, entry count and bytes used where those fields apply.

The scanner discovers metadata only. It does not yet reconstruct paths from historical directory/index combinations, recover file payloads, resolve competing historical graphs or write repaired structures.
