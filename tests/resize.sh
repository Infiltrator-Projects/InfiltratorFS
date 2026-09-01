#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

build="${1:?build directory required}"
mkfs="$build/mkfs.infilfs"
tool="$build/infilfs-tool"
scrub="$build/infilfs-scrub"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
image="$work/resize.img"
payload="$work/payload.bin"
readback="$work/readback.bin"

truncate -s 128M "$image"
"$mkfs" --force -L resize-test "$image"

# Empty Format 0.17 geometry can safely shrink: allocation-tree/checkpoint
# bookkeeping is relocated below the new boundary before block zero commits it.
"$tool" "$image" resize 64MiB | grep -Fq 'resized=67108864'

# The shrunken image must be reopenable, then grow online-within-backing-store
# semantics back to the full 128 MiB physical image.
"$tool" "$image" resize max | grep -Fq 'resized=134217728'

python3 - "$payload" <<'PY'
import sys
with open(sys.argv[1], "wb") as f:
    f.write((b"InfiltratorFS-resize-regression\n" * 8192)[:196608])
PY
"$tool" "$image" put "$payload" /payload.bin
"$tool" "$image" cat /payload.bin > "$readback"
cmp "$payload" "$readback"

# Named snapshots intentionally pin historical geometry in Format 0.17.
"$tool" "$image" snapshot-create resize-held
if "$tool" "$image" resize 96MiB >"$work/snapshot.out" 2>"$work/snapshot.err"; then
    echo "resize unexpectedly succeeded with a named snapshot" >&2
    exit 1
fi
grep -Fqi 'busy' "$work/snapshot.err"
"$tool" "$image" snapshot-delete resize-held

# After real mutations the high metadata arena may occupy the requested tail.
# Shrink must fail closed rather than silently relocate references it cannot yet
# prove safe.  Data must remain intact after the rejected operation.
if "$tool" "$image" resize 96MiB >"$work/occupied.out" 2>"$work/occupied.err"; then
    echo "resize unexpectedly succeeded across allocated tail blocks" >&2
    exit 1
fi
grep -Fqi 'busy' "$work/occupied.err"
"$tool" "$image" cat /payload.bin > "$readback"
cmp "$payload" "$readback"

"$scrub" "$image" | grep -Fq 'Result:              CLEAN'
echo "resize qualification: ok"
