#!/usr/bin/env bash
set -euo pipefail
root="${1:-.}"

grep -Fq '.tmpfile = infilfs_posix_acl_tmpfile' "$root/kernel/infiltratorfs_rw.inc"
grep -Fq 'RENAME_EXCHANGE' "$root/kernel/infiltratorfs_rw_namespace.inc"
grep -Fq 'infs_compression_metrics' "$root/include/infilfs/volume.h"
grep -Fq 'infilfs-compression' "$root/CMakeLists.txt"
grep -Fq 'INFS_COMPRESSION_LZ4' "$root/include/infilfs/format.h"

# Fast root-volume readiness remains an automatic release qualification.
grep -Fq 'Linux root-volume qualification' "$root/.github/workflows/release-packages.yml"

# Real root boot is deliberately heavyweight. It must stay manual-only and
# must never become an automatic release prerequisite.
grep -Fq 'workflow_dispatch:' "$root/.github/workflows/root-boot-qualification.yml"
root_boot_on_block="$(sed -n '/^on:/,/^permissions:/p' "$root/.github/workflows/root-boot-qualification.yml")"
if grep -Eq '^[[:space:]]+(push|schedule|pull_request|workflow_run):' <<<"$root_boot_on_block"; then
    echo 'Root boot qualification must remain manual-only.' >&2
    exit 1
fi
if grep -Fq '"Linux root boot qualification"' "$root/.github/workflows/release-packages.yml"; then
    echo 'Release workflow must not wait for heavyweight root boot qualification.' >&2
    exit 1
fi

grep -Fq 'ROOT_RECOVERY_PASS' "$root/tests/root-boot-qemu.sh"
! grep -R -Fq 'INFS_IAC1_MIN_SAVINGS_DIVISOR' \
    "$root/kernel" "$root/src" "$root/include" || exit 1

echo 'Root readiness policy: PASS'
