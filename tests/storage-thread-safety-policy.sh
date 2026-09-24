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

grep -Fq 'struct infs_storage_lock state_lock;' "$mirror" ||     fail 'mirror member state lock missing'
grep -Fq 'mirror_member_healthy' "$mirror" ||     fail 'mirror health reads bypass synchronized helper'
grep -Fq 'mirror_quarantine_member' "$mirror" ||     fail 'mirror quarantine writes bypass synchronized helper'
if grep -Fq 'ctx->healthy[i]' "$mirror"; then
    fail 'mirror I/O loop directly races on member health state'
fi

echo "storage thread-safety policy: PASS"
