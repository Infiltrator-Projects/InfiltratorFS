#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
lock="$root/src/storage_lock.h"
encrypted="$root/src/storage_encrypted.c"
mirror="$root/src/storage_mirror.c"

fail() {
    echo "storage thread-safety policy: $*" >&2
    exit 1
}

for file in "$lock" "$encrypted" "$mirror"; do
    test -f "$file" || fail "missing $file"
done

grep -Fq 'pthread_mutex_t native;' "$lock" ||     fail 'POSIX storage lock is not a blocking mutex'
grep -Fq 'SRWLOCK native;' "$lock" ||     fail 'Windows storage lock is not an SRW lock'

grep -Fq 'block_locks[ENC_LOCK_STRIPES]' "$encrypted" ||     fail 'encrypted storage lost per-block stripe locks'
grep -Fq '&ctx->block_locks[logical % ENC_LOCK_STRIPES]' "$encrypted" ||     fail 'encrypted I/O no longer selects a logical-block stripe'
grep -Fq 'infs_storage_lock_acquire(lock);' "$encrypted" ||     fail 'encrypted read-modify-write is not serialized'
grep -Fq 'for (size_t i = 0; i < ENC_LOCK_STRIPES; ++i)' "$encrypted" ||     fail 'encrypted durability barrier no longer fences every stripe'

grep -Fq 'struct infs_storage_lock operation_lock;' "$mirror" || fail 'mirror operation lock missing'
grep -Fq 'mirror_read_policy_unlocked' "$mirror" || fail 'mirror read not fenced'
grep -Fq 'mirror_write_policy_unlocked' "$mirror" || fail 'mirror write not fenced'
grep -Fq 'mirror_flush_unlocked' "$mirror" || fail 'mirror flush not fenced'
grep -Fq 'infs_storage_lock_acquire(&ctx->operation_lock);' "$mirror" || fail 'mirror I/O cannot establish serialization'
grep -Fq 'infs_storage_lock_release(&ctx->operation_lock);' "$mirror" || fail 'mirror operation lock is not released'

echo "storage thread-safety policy: PASS"
