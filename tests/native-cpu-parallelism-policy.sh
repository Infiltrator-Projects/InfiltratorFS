#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"
core="$root/kernel/infiltratorfs_core.c"
internal="$root/kernel/infiltratorfs_internal.h"
data="$root/kernel/infiltratorfs_rw_data.inc"
read_cache="$root/kernel/infiltratorfs_read_cache.c"
pagecache="$root/kernel/infiltratorfs_pagecache.c"
makefile="$root/kernel/Makefile"
ioctl="$root/kernel/infiltratorfs_ioctl.h"
architecture="$root/docs/ARCHITECTURE.md"
roadmap="$root/docs/ROADMAP.md"

fail() {
    echo "native CPU parallelism policy: $*" >&2
    exit 1
}

for file in "$core" "$internal" "$data" "$read_cache" "$pagecache" \
            "$makefile" "$ioctl" "$architecture" "$roadmap"; do
    test -f "$file" || fail "missing $file"
done

# The filesystem-wide execution ceiling is exactly N-1 online logical CPUs,
# except that a one-CPU machine must still be able to execute filesystem work.
grep -Fq 'unsigned int online_logical_cpus = num_online_cpus();' "$core" || \
    fail 'CPU budget is not derived from online logical CPUs'
grep -Fq 'online_logical_cpus > 1u ? online_logical_cpus - 1u : 1u' "$core" || \
    fail 'CPU budget is not max(1, online logical CPUs - 1)'
grep -Fq 'num_possible_cpus()' "$core" || \
    fail 'CPU pool cannot grow when an offline possible CPU is later onlined'
grep -Fq 'WQ_UNBOUND | WQ_MEM_RECLAIM' "$core" || \
    fail 'native CPU pool lost unbound/reclaim-safe workqueue semantics'
grep -Fq 'atomic_cmpxchg(&infilfs_cpu_active' "$core" || \
    fail 'module-wide execution gate is missing'
grep -Fq 'wait_event(infilfs_cpu_wait, infilfs_cpu_try_enter())' "$core" || \
    fail 'CPU work no longer waits for the module-wide N-1 gate'
grep -Fq 'filesystem_budget=%u reserved_for_os=%u' "$core" || \
    fail 'mounted evidence no longer reports the CPU policy'

# The code path and its shipped/DKMS synchronization contract must agree with
# the architecture documentation. The roadmap remains incomplete until full
# mutation/publication qualification proves the whole write path scales.
grep -Fq 'filesystem_cpu_budget = max(1, online_logical_cpus - 1)' "$architecture" || \
    fail 'architecture lost the normative N-1 formula'
grep -Fq 'max(1, online logical CPUs - 1)' "$makefile" || \
    fail 'Kbuild synchronization contract lost N-1 policy'
grep -Fq 'max(1, online logical CPUs - 1)' "$ioctl" || \
    fail 'DKMS synchronization contract lost N-1 policy'
grep -Fq -- '- [ ] Filesystem-wide native Linux concurrency budget of `max(1, online logical CPUs - 1)`' "$roadmap" || \
    fail 'roadmap must stay open until full N-1 mutation scaling is qualified'

# CPU-heavy native preparation and verified-read hashing use the shared pool,
# never generic system_unbound_wq, and must not regress to arbitrary four/eight
# worker caps. The buffered writeback window scales with that same budget so a
# single large dirty stream can expose enough independent compression clusters.
grep -Fq 'infilfs_native_writeback_batch_bytes' "$internal" || \
    fail 'dynamic N-1 writeback batch helper missing'
grep -Fq 'infilfs_cpu_budget()' "$internal" || \
    fail 'writeback batch is not CPU-budget-aware'
grep -Fq 'infilfs_queue_cpu_work(&items[i].work)' "$data" || \
    fail 'write preparation bypasses native CPU pool'
grep -Fq 'if (!infilfs_queue_cpu_work(&items[i].work))' "$data" || \
    fail 'write preparation queue failure can strand a completion'
grep -Fq 'items[i].ret = infilfs_native_prepare_append(' "$data" || \
    fail 'write preparation queue failure lost its synchronous fallback'
grep -Fq 'complete(&items[i].done);' "$data" || \
    fail 'write preparation queue failure does not complete its waiter'
grep -Fq 'prepared_append_peak_active' "$internal" || \
    fail 'real prepared-write concurrency telemetry missing'
grep -Fq 'infilfs_native_prepare_parallel_enter(sbi);' "$data" || \
    fail 'prepared-write concurrency is not measured inside real worker execution'
grep -Fq 'atomic64_dec(&sbi->prepared_append_active);' "$data" || \
    fail 'prepared-write active telemetry is not balanced'
grep -Fq 'prepared_append_peak_active=%lld' "$root/kernel/infiltratorfs_parallel_alloc.c" || \
    fail 'mounted prepared-write peak evidence is not reported'
grep -Fq 'infilfs_cpu_work_enter();' "$data" || \
    fail 'write preparation does not consume a native CPU slot'
grep -Fq 'infilfs_queue_cpu_work(&work[i].work)' "$read_cache" || \
    fail 'verified-read hashing bypasses native CPU pool'
grep -Fq 'workers = min_t(u32, infilfs_cpu_budget(), blocks);' "$read_cache" || \
    fail 'verified-read hashing is not bounded by N-1 policy'
grep -Fq 'size_t batch_bytes = infilfs_native_writeback_batch_bytes();' "$pagecache" || \
    fail 'buffered writeback still uses a fixed preparation window'
! grep -Fq 'INFILFS_NATIVE_PREP_BATCH_MAX 4u' "$data" || \
    fail 'four-worker write preparation cap returned'
! grep -Fq 'INFILFS_NATIVE_READ_HASH_WORKERS 4u' "$read_cache" || \
    fail 'four-worker read hash cap returned'
! grep -Fq 'system_unbound_wq' "$data" || \
    fail 'write preparation regressed to generic system_unbound_wq'
! grep -Fq 'system_unbound_wq' "$read_cache" || \
    fail 'read hashing regressed to generic system_unbound_wq'
! grep -Fq 'mod_delayed_work(system_wq, &pending->idle_work' "$data" || \
    fail 'idle publication regressed to system_wq'
grep -Fq 'infilfs_mod_delayed_cpu_work(&pending->idle_work' "$data" || \
    fail 'idle publication bypasses native unbound CPU pool'
grep -Fq 'infilfs_cpu_work_enter();' "$data" || \
    fail 'idle publication/write preparation lost N-1 execution gating'
grep -Fq 'infilfs_mod_delayed_cpu_work(&sbi->orphan_recovery_work, 1);' "$core" || \
    fail 'orphan recovery bypasses native unbound CPU pool'
grep -Fq 'infilfs_cpu_work_enter();' "$core" || \
    fail 'orphan recovery lost N-1 execution gating'
! grep -Fq 'mod_delayed_work(system_long_wq, &pending->idle_work' "$data" || \
    fail 'idle publication regressed to system_long_wq'

python3 - <<'PY'
def budget(n):
    return n - 1 if n > 1 else 1

expected = {1: 1, 2: 1, 3: 2, 14: 13, 255: 254}
for cpus, wanted in expected.items():
    got = budget(cpus)
    if got != wanted:
        raise SystemExit(f"{cpus} CPUs -> {got}, expected {wanted}")
PY

printf 'Native N-1 CPU parallelism policy guard passed.\n'
