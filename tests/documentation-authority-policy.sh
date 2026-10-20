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
    fail 'IAC1 hardware note contradicts retained Format 0.19 LZ4 support'
fi
grep -Fq 'codec identifier 1 for LZ4' docs/IAC1-2026-HARDWARE-DESIGN.md || \
    fail 'IAC1 hardware note omits retained Format 0.19 LZ4 support'

# Live automatic evidence belongs to workflow definitions and exact-SHA Actions
# results; VALIDATION must name that executable authority instead of becoming a
# manually synchronised second workflow inventory.
grep -Fq '.github/workflows/' docs/VALIDATION.md || \
    fail 'VALIDATION does not identify the executable workflow authority'
if grep -Eq '\.github/workflows/[A-Za-z0-9_.-]+\.ya?ml' docs/VALIDATION.md; then
    fail 'VALIDATION duplicates individual workflow filenames instead of the live authority'
fi


# Derive current format identity from the byte-level authority, not from the
# software release number or a duplicated documentation constant.
major="$(sed -nE 's/^#define INFS_FORMAT_MAJOR ([0-9]+)u$/\1/p' include/infilfs/format.h)"
minor="$(sed -nE 's/^#define INFS_FORMAT_MINOR ([0-9]+)u$/\1/p' include/infilfs/format.h)"
[[ "$major" =~ ^[0-9]+$ && "$minor" =~ ^[0-9]+$ ]] ||
    fail 'canonical format version is missing'
current_format="$major.$minor"
grep -Fq "# InfiltratorFS On-Disk Format $current_format" docs/ON_DISK_FORMAT.md ||
    fail 'normative on-disk document contradicts canonical format version'
grep -Fq "persistent Format $current_format structural contract" "$map" ||
    fail 'documentation authority map names an obsolete format'
# ADRs and historical qualification remain historical; the following files
# purport to describe the live current-format contract.
for normative in docs/COMPRESSION.md docs/SECURITY.md docs/FORENSICS.md docs/INSPIRATIONS.md kernel/README.md kernel/infiltratorfs_rw.inc kernel/infiltratorfs_rw_data.inc include/infilfs/format.h; do
    obsolete="$(grep -Eo 'Format [0-9]+\.[0-9]+' "$normative" | sort -u | grep -Fvx "Format $current_format" || true)"
    [[ -z "$obsolete" ]] || fail "$normative names obsolete $obsolete as a current-format contract"
done

printf 'Documentation authority policy: PASS\n'
