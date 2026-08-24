<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Forensic metadata scanning

`infilfs-forensic` is a read-only physical-block scanner introduced in implementation 0.11.0. It does not require the current namespace graph to open and does not modify, repair, mount or replay the target.

```bash
infilfs-forensic volume.img
infilfs-forensic --jsonl /dev/sdb1 > evidence.jsonl
```

The scanner authenticates each recognized record independently using the checks already defined by Format 0.9, including native symbolic-link objects:

- checkpoint structure, geometry-independent fields and CRC64;
- object magic, type/version, identity, payload shape, canonical padding and CRC64;
- directory-page ownership, generation, bounds, canonical padding and CRC64; and
- index-page ownership, generation, bounds, canonical padding and CRC64.

Arbitrary data that merely contains a magic string is not reported. A candidate must satisfy its complete block-level validation contract.

## Record state

When a valid checkpoint and readable bitmap survive, the scanner reports:

- `current` for metadata allocated by the selected live generation and checkpoint replicas matching that generation;
- `stale` for an independently valid older checkpoint replica; and
- `orphaned` for an authenticated object or page that is no longer allocated by the live bitmap, commonly a superseded CoW record.

When no trustworthy current checkpoint/bitmap pair is available, authenticated raw records are still reported with state `unknown`. This is deliberate: raw discovery remains possible without claiming that a record belongs to an authoritative namespace.

## Output contract

The default output is tab-separated and ends with a summary row. `--jsonl` emits one JSON object per discovered record followed by a summary object. Each metadata row includes its physical block, classification, kind, generation, object type/version, object and parent identities, payload size, entry count and bytes used where those fields apply.

The scanner discovers metadata only. It does not yet reconstruct paths from historical directory/index combinations, recover file payloads, resolve competing historical graphs or write repaired structures.
