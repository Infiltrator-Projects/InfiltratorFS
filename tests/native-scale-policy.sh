#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
harness="$root/tests/native-scale-qualification.sh"
stress="$root/tests/native-scale-stress.py"

[[ -f "$harness" && -f "$stress" ]]
grep -Fq 'INFS_SCALE_FILE_COUNT:-1000000' "$harness"
grep -Fq 'INFS_SCALE_LARGE_IMAGE_SIZE:-1T' "$harness"
grep -Fq 'INFS_SCALE_CHURN_FILES:-100000' "$harness"
grep -Fq 'INFS_SCALE_BATCH_DIRECTORIES:-64' "$harness"
grep -Fq -- '--reclaim-vfs-cache' "$harness"
grep -Fq -- '--verify-only' "$harness"
grep -Fq 'Native million-file and 1 TiB mounted scale qualification: PASS' "$harness"
grep -Fq 'ProcessPoolExecutor' "$stress"
grep -Fq 'run_parallel_batched' "$stress"
grep -Fq 'drop_caches' "$stress"
grep -Fq 'verify_population' "$stress"
grep -Fq '[SCALE-PERF]' "$stress"

echo 'Native million-file/large-volume scale policy guard passed.'
