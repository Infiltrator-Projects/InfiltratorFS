#!/usr/bin/env bash
set -euo pipefail
root="${1:-.}"
grep -Fq '.tmpfile = infilfs_posix_acl_tmpfile' "$root/kernel/infiltratorfs_rw.inc"
grep -Fq 'RENAME_EXCHANGE' "$root/kernel/infiltratorfs_rw_namespace.inc"
grep -Fq 'infs_compression_metrics' "$root/include/infilfs/volume.h"
grep -Fq 'infilfs-compression' "$root/CMakeLists.txt"
grep -Fq 'INFS_COMPRESSION_LZ4' "$root/include/infilfs/format.h"
grep -Fq 'Linux root boot qualification' "$root/.github/workflows/release-packages.yml"
grep -Fq 'Linux root-volume qualification' "$root/.github/workflows/release-packages.yml"
grep -Fq 'ROOT_RECOVERY_PASS' "$root/tests/root-boot-qemu.sh"
! grep -R -Fq 'INFS_IAC1_MIN_SAVINGS_DIVISOR' \
    "$root/kernel" "$root/src" "$root/include" || exit 1
echo 'Root readiness policy: PASS'
