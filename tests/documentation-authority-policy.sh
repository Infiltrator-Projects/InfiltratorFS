#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
cd "$root"

fail() {
    echo "documentation policy: $*" >&2
    exit 1
}

map=docs/README.md
[[ -f "$map" ]] || fail 'documentation authority map is missing'

# Every maintained top-level documentation file must have exactly one visible
# place in the authority map. Historical per-release notes are intentionally
# covered by the RELEASE-*.md class rather than enumerated one by one.
while IFS= read -r path; do
    name="${path#docs/}"
    [[ "$name" == README.md ]] && continue
    if [[ "$name" == RELEASE-*.md ]]; then
        grep -Fq '`RELEASE-*.md`' "$map" || \
            fail 'release-history class is missing from the documentation map'
        continue
    fi
    grep -Fq "\`${name}\`" "$map" || \
        fail "${name} is not classified by docs/README.md"
done < <(find docs -maxdepth 1 -type f -name '*.md' -print | sort)

# Specialist performance material may narrow implementation guidance but may
# not redefine persistent codec semantics owned by COMPRESSION/ON_DISK_FORMAT.
if grep -Fq 'IAC1 is the only compressed on-disk representation' \
        docs/IAC1-2026-HARDWARE-DESIGN.md; then
    fail 'IAC1 hardware note contradicts retained Format 0.18 LZ4 support'
fi
grep -Fq 'codec identifier 1 for LZ4' docs/IAC1-2026-HARDWARE-DESIGN.md || \
    fail 'IAC1 hardware note omits retained Format 0.18 LZ4 support'

# Live automatic evidence belongs to workflow definitions and exact-SHA Actions
# results; VALIDATION must name that executable authority instead of becoming a
# manually synchronised second workflow inventory.
grep -Fq '.github/workflows/' docs/VALIDATION.md || \
    fail 'VALIDATION does not identify the executable workflow authority'
if grep -Eq '\.github/workflows/[A-Za-z0-9_.-]+\.ya?ml' docs/VALIDATION.md; then
    fail 'VALIDATION duplicates individual workflow filenames instead of the live authority'
fi

printf 'Documentation authority policy: PASS\n'
