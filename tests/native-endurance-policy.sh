#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
harness="$root/tests/native-endurance-qualification.sh"
stress="$root/tests/native-endurance-stress.py"

[[ -f "$harness" && -f "$stress" ]]

grep -Fq 'INFS_ENDURANCE_SECONDS:-300' "$harness"
grep -Fq 'INFS_ENDURANCE_IMAGE_SIZE:-4G' "$harness"
grep -Fq 'INFS_ENDURANCE_RESERVE_MIB:-384' "$harness"
grep -Fq 'Native near-full, fragmentation and long-running mixed workload qualification: PASS' "$harness"

grep -Fq 'fill_fragmented' "$stress"
grep -Fq 'ProcessPoolExecutor' "$stress"
grep -Fq 'os.setxattr' "$stress"
grep -Fq 'os.ftruncate' "$stress"
grep -Fq 'os.link' "$stress"
grep -Fq 'os.symlink' "$stress"
grep -Fq 'verify_manifest' "$stress"
grep -Fq '[ENDURANCE-PERF]' "$stress"

echo 'Native near-full/fragmentation/endurance policy guard passed.'
