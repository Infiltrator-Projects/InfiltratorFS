#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Defensive static parity guard. Native kernel compilation and actual
# cross-platform byte fixtures are separate qualification requirements.
set -euo pipefail
root="${1:?repository root required}"
h="$root/kernel/infiltratorfs_locator_format.h"
c="$root/kernel/infiltratorfs_locator_format.c"
p="$root/include/infilfs/locator.h"
make="$root/kernel/Makefile"
internal="$root/kernel/infiltratorfs_internal.h"
[[ -f "$h" && -f "$c" && -f "$p" ]]
grep -Fq 'infiltratorfs_locator_format.o' "$make"
grep -Fq '#include "infiltratorfs_locator_format.h"' "$internal"
python3 - "$h" "$p" "$c" <<'PY'
from pathlib import Path
import re
import sys

native = Path(sys.argv[1]).read_text()
portable = Path(sys.argv[2]).read_text()
implementation = Path(sys.argv[3]).read_text()
constants = [
    ('INFILFS_LOCATOR_FORMAT_MINOR', 'INFS_LOCATOR_FORMAT_MINOR'),
    ('INFILFS_LOCATOR_SCHEMA_VERSION', 'INFS_LOCATOR_SCHEMA_VERSION'),
    ('INFILFS_LOCATOR_BLOCK_A', 'INFS_LOCATOR_BLOCK_A'),
    ('INFILFS_LOCATOR_BLOCK_B', 'INFS_LOCATOR_BLOCK_B'),
    ('INFILFS_LOCATOR_MIN_VOLUME_BLOCKS', 'INFS_LOCATOR_MIN_VOLUME_BLOCKS'),
    ('INFILFS_LOCATOR_HEADER_BYTES', 'INFS_LOCATOR_HEADER_BYTES'),
]
def extract(txt, name):
    m = re.search(r'^#define\s+' + re.escape(name) + r'\s+([^\s]+)', txt, re.M)
    if not m:
        raise SystemExit(f'missing {name}')
    v = m.group(1).replace('UINT64_C(', '').replace(')', '').rstrip('uUlL')
    return int(v, 0)

for k, u in constants:
    if extract(native, k) != extract(portable, u):
        raise SystemExit(f'Format 0.19 portable/native locator mismatch: {k} {u}')
if 'BUILD_BUG_ON(sizeof(struct infilfs_locator_disk)' not in implementation:
    raise SystemExit('native packed size is not compile-time asserted')
if 'offsetof(struct infilfs_locator_disk, checksum) != 72u' not in implementation:
    raise SystemExit('native CRC64 offset is not compile-time asserted')
for rule in ('infilfs_crc64_block_valid(', 'memchr_inv(block + sizeof(*disk)', 
             'candidate.total_blocks > device_blocks',
             'infilfs_locator_shape_valid(&candidate)'):
    if rule not in implementation:
        raise SystemExit(f'native CRC/geometry rule missing: {rule}')
print('Format 0.19 native/portable locator-source parity: PASS')
PY

# Native recovery must use fixed locators and the shared writable
# publication protocol, including the installed DKMS source manifest.
core="$root/kernel/infiltratorfs_core.c"
discovery="$root/kernel/infiltratorfs_checkpoint_discovery.c"
[[ -f "$discovery" ]]
grep -Fq 'infilfs_locator_decode_019(' "$discovery"
grep -Fq 'INFILFS_RECOVERY_CANDIDATE_MAX' "$root/kernel/infiltratorfs_internal.h"
grep -Fq 'infs_checkpoint_publish(' "$root/kernel/infiltratorfs_checkpoint_publish.c"
grep -Fq 'infs_checkpoint_publish(' "$root/src/volume/checkpoint-publication.inc"
grep -Fq 'infs_checkpoint_publish(' "$root/src/volume/resize.inc"
grep -Fq 'infilfs_checkpoint_publish(' "$root/kernel/infiltratorfs_resize.c"
grep -Fq 'include/infilfs/checkpoint_protocol.h' "$root/packaging/kernel-dkms-files.list"
