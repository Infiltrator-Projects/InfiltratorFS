// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Volatile sharded allocation reservations for the native Linux adapter.
 *
 * The committed allocation bitmap remains authoritative.  Writers may reserve
 * a free data run concurrently in one of 64 independently locked arenas before
 * entering write_lock.  Transaction claim atomically converts that reservation
 * into ordinary allocation-journal entries; abort/rollback therefore uses the
 * existing CoW transaction machinery unchanged.  Reservations are never
 * persisted and cannot make an uncommitted block reachable after a crash.
 */

static bool infilfs_parallel_bitmap_get(const unsigned long *bitmap, u64 block)
{
    return test_bit((unsigned long)block, bitmap);
}

static void infilfs_parallel_bitmap_set(unsigned long *bitmap, u64 block,
                                        bool used)
{
    if (used)
        set_bit((unsigned long)block, bitmap);
    else
        clear_bit((unsigned long)block, bitmap);
}

static u32 infilfs_parallel_shard_for(const struct infilfs_sb_info *sbi,
                                      u64 block)
{
    u64 allocatable;
    u64 offset;

    if (!sbi || sbi->device_blocks <= 1u || block <= 1u)
        return 0;
    allocatable = sbi->device_blocks - 1u;
    offset = min_t(u64, block - 1u, allocatable - 1u);
    return min_t(u32,
        (u32)div64_u64(offset * INFILFS_ALLOCATION_RESERVATION_SHARDS,
                       allocatable),
        INFILFS_ALLOCATION_RESERVATION_SHARDS - 1u);
}

static void infilfs_parallel_shard_bounds(const struct infilfs_sb_info *sbi,
                                          u32 shard, u64 *start, u64 *end)
{
    u64 allocatable = sbi->device_blocks > 1u ?
        sbi->device_blocks - 1u : 0;

    *start = 1u + div64_u64(
        allocatable * shard, INFILFS_ALLOCATION_RESERVATION_SHARDS);
    *end = 1u + div64_u64(
        allocatable * (shard + 1u),
        INFILFS_ALLOCATION_RESERVATION_SHARDS);
}

static u64 infilfs_parallel_object_preferred(
    const struct infilfs_sb_info *sbi, const u8 object_id[16])
{
    u64 hash = 1469598103934665603ULL;
    u64 start;
    u64 end;
    unsigned int i;

    for (i = 0; i < 16u; ++i) {
        hash ^= object_id[i];
        hash *= 1099511628211ULL;
    }
    infilfs_parallel_shard_bounds(
        sbi, (u32)(hash % INFILFS_ALLOCATION_RESERVATION_SHARDS),
        &start, &end);
    return start < end ? start : 1u;
}

static void infilfs_parallel_lock_range(struct infilfs_sb_info *sbi,
                                        u32 first, u32 last)
{
    u32 shard;

    for (shard = first; shard <= last; ++shard)
        spin_lock(&sbi->allocation_reservation_locks[shard]);
}

static void infilfs_parallel_unlock_range(struct infilfs_sb_info *sbi,
                                          u32 first, u32 last)
{
    u32 shard = last;

    for (;;) {
        spin_unlock(&sbi->allocation_reservation_locks[shard]);
        if (shard == first)
            break;
        shard--;
    }
}

static bool infilfs_parallel_range_reserved(
    const struct infilfs_sb_info *sbi, u64 start, u64 count)
{
    u64 block;

    if (!sbi || !sbi->allocation_reservations || !count ||
        start >= sbi->device_blocks || count > sbi->device_blocks - start)
        return false;
    for (block = start; block < start + count; ++block)
        if (infilfs_parallel_bitmap_get(sbi->allocation_reservations, block))
            return true;
    return false;
}

static int infilfs_parallel_allocator_mount_init(struct super_block *sb)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    u64 words;
    u32 shard;

    for (shard = 0; shard < INFILFS_ALLOCATION_RESERVATION_SHARDS; ++shard) {
        u64 start;
        u64 end;

        spin_lock_init(&sbi->allocation_reservation_locks[shard]);
        infilfs_parallel_shard_bounds(sbi, shard, &start, &end);
        sbi->allocation_reservation_hints[shard] = start;
    }
    atomic64_set(&sbi->allocation_reservation_steer, 0);
    atomic64_set(&sbi->allocation_reserved_blocks, 0);
    atomic64_set(&sbi->allocation_active_reservations, 0);
    atomic64_set(&sbi->allocation_peak_active_reservations, 0);
    atomic64_set(&sbi->allocation_reservation_successes, 0);
    atomic64_set(&sbi->allocation_reservation_conflicts, 0);
    atomic64_set(&sbi->allocation_workload_sequential, 0);
    atomic64_set(&sbi->allocation_workload_random, 0);
    atomic64_set(&sbi->allocation_workload_sparse, 0);
    atomic64_set(&sbi->allocation_locality_scored, 0);
    atomic64_set(&sbi->allocation_best_fit, 0);

    if (sb_rdonly(sb))
        return 0;
    words = DIV_ROUND_UP_ULL(sbi->device_blocks, BITS_PER_LONG);
    if (words > SIZE_MAX / sizeof(unsigned long))
        return -EOVERFLOW;
    sbi->allocation_reservation_bytes =
        (size_t)words * sizeof(unsigned long);
    sbi->allocation_reservations = kvzalloc(
        sbi->allocation_reservation_bytes, GFP_KERNEL);
    if (!sbi->allocation_reservations)
        return -ENOMEM;
    return 0;
}

static void infilfs_parallel_allocator_mount_destroy(struct super_block *sb)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);

    if (!sbi)
        return;
    if (sbi->allocation_reservations)
        pr_info("InfiltratorFS: allocator reservations=%lld peak_active=%lld conflicts=%lld workload_seq=%lld workload_random=%lld workload_sparse=%lld locality_scored=%lld best_fit=%lld\n",
                (long long)atomic64_read(
                    &sbi->allocation_reservation_successes),
                (long long)atomic64_read(
                    &sbi->allocation_peak_active_reservations),
                (long long)atomic64_read(
                    &sbi->allocation_reservation_conflicts),
                (long long)atomic64_read(
                    &sbi->allocation_workload_sequential),
                (long long)atomic64_read(
                    &sbi->allocation_workload_random),
                (long long)atomic64_read(
                    &sbi->allocation_workload_sparse),
                (long long)atomic64_read(
                    &sbi->allocation_locality_scored),
                (long long)atomic64_read(
                    &sbi->allocation_best_fit));
    kvfree(sbi->allocation_reservations);
    sbi->allocation_reservations = NULL;
    sbi->allocation_reservation_bytes = 0;
    atomic64_set(&sbi->allocation_reserved_blocks, 0);
}

static int infilfs_parallel_alloc_journal_reserve(struct infilfs_rw_tx *tx)
{
    struct infilfs_rw_free_range *grown;
    size_t next;

    if (tx->allocated_count < tx->allocated_capacity)
        return 0;
    next = tx->allocated_capacity ? tx->allocated_capacity * 2u :
        INFILFS_RW_ALLOC_RANGES_INITIAL;
    if (next < tx->allocated_capacity ||
        next > SIZE_MAX / sizeof(*grown))
        return -EOVERFLOW;
    grown = krealloc(tx->allocated, next * sizeof(*grown), GFP_NOFS);
    if (!grown)
        return -ENOMEM;
    tx->allocated = grown;
    tx->allocated_capacity = next;
    return 0;
}

static int infilfs_parallel_tx_claim(struct infilfs_rw_tx *tx, u64 start,
                                     u64 count, bool consume_reservation)
{
    struct infilfs_sb_info *sbi;
    u32 first;
    u32 last;
    u64 block;
    int ret;

    if (!tx || !tx->sbi || !tx->bitmap || !count)
        return -EINVAL;
    sbi = tx->sbi;
    if (start < 1u || start >= sbi->device_blocks ||
        count > sbi->device_blocks - start || count > tx->free_blocks)
        return -ENOSPC;
    ret = infilfs_parallel_alloc_journal_reserve(tx);
    if (ret)
        return ret;

    first = infilfs_parallel_shard_for(sbi, start);
    last = infilfs_parallel_shard_for(sbi, start + count - 1u);
    infilfs_parallel_lock_range(sbi, first, last);
    write_lock(&sbi->bitmap_lock);
    for (block = start; block < start + count; ++block) {
        bool reserved = sbi->allocation_reservations &&
            infilfs_parallel_bitmap_get(
                sbi->allocation_reservations, block);

        if (infilfs_rw_bitmap_get(tx->bitmap, block) ||
            reserved != consume_reservation) {
            ret = reserved && !consume_reservation ? -EAGAIN : -EEXIST;
            goto out_unlock;
        }
    }

    for (block = start; block < start + count; ++block) {
        infilfs_rw_bitmap_set(tx->bitmap, block, true);
        if (consume_reservation)
            infilfs_parallel_bitmap_set(
                sbi->allocation_reservations, block, false);
    }
    tx->allocated[tx->allocated_count].start = start;
    tx->allocated[tx->allocated_count].count = count;
    tx->allocated_count++;
    tx->free_blocks -= count;
    ret = 0;

out_unlock:
    write_unlock(&sbi->bitmap_lock);
    infilfs_parallel_unlock_range(sbi, first, last);
    if (ret) {
        if (ret == -EAGAIN)
            atomic64_inc(&sbi->allocation_reservation_conflicts);
        return ret;
    }
    if (consume_reservation)
        atomic64_sub(count, &sbi->allocation_reserved_blocks);
    if (tx->free_extent_index_valid &&
        infilfs_rw_free_extent_index_remove(tx, start, count) != 0)
        infilfs_rw_free_extent_index_invalidate(tx);
    return 0;
}

static bool infilfs_parallel_range_free_locked(
    const struct infilfs_sb_info *sbi, u64 start, u64 count)
{
    u64 block;

    for (block = start; block < start + count; ++block)
        if (infilfs_rw_bitmap_get(sbi->bitmap, block) ||
            infilfs_parallel_bitmap_get(sbi->allocation_reservations, block))
            return false;
    return true;
}

static void infilfs_parallel_note_workload(
    struct infilfs_sb_info *sbi, enum infilfs_data_workload workload)
{
    if (!sbi)
        return;
    switch (workload) {
    case INFILFS_DATA_WORKLOAD_SEQUENTIAL:
        atomic64_inc(&sbi->allocation_workload_sequential);
        break;
    case INFILFS_DATA_WORKLOAD_SPARSE:
        atomic64_inc(&sbi->allocation_workload_sparse);
        break;
    case INFILFS_DATA_WORKLOAD_RANDOM:
    default:
        atomic64_inc(&sbi->allocation_workload_random);
        break;
    }
}

static void infilfs_parallel_note_active(struct infilfs_sb_info *sbi)
{
    s64 active = atomic64_inc_return(&sbi->allocation_active_reservations);
    s64 peak = atomic64_read(&sbi->allocation_peak_active_reservations);

    while (active > peak) {
        s64 observed = atomic64_cmpxchg(
            &sbi->allocation_peak_active_reservations, peak, active);

        if (observed == peak)
            break;
        peak = observed;
    }
}

static int infilfs_parallel_reserve_data(
    struct super_block *sb, u64 count, u64 preferred,
    struct infilfs_parallel_reservation *reservation)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    u64 metadata_reserve;
    s64 charged;
    u32 first_shard;
    u32 pass;

    memset(reservation, 0, sizeof(*reservation));
    if (!sbi || !sbi->allocation_reservations || !count ||
        count >= sbi->device_blocks)
        return -ENOSPC;
    metadata_reserve = infilfs_native_metadata_reserve_blocks(sbi);
    for (;;) {
        u64 free_blocks = infilfs_native_visible_free_blocks(sb);
        u64 available;

        charged = atomic64_read(&sbi->allocation_reserved_blocks);
        available = free_blocks > metadata_reserve ?
            free_blocks - metadata_reserve : 0;
        if ((u64)charged >= available || count > available - (u64)charged)
            return -ENOSPC;
        if (atomic64_cmpxchg(&sbi->allocation_reserved_blocks,
                             charged, charged + (s64)count) == charged)
            break;
    }
    first_shard = preferred > 0 && preferred < sbi->device_blocks ?
        infilfs_parallel_shard_for(sbi, preferred) :
        (u32)(atomic64_inc_return(&sbi->allocation_reservation_steer) %
              INFILFS_ALLOCATION_RESERVATION_SHARDS);

    for (pass = 0; pass < INFILFS_ALLOCATION_RESERVATION_SHARDS; ++pass) {
        u32 shard = (first_shard + pass) %
            INFILFS_ALLOCATION_RESERVATION_SHARDS;
        u64 arena_start;
        u64 arena_end;
        u64 hint;
        unsigned int sweep;

        infilfs_parallel_shard_bounds(
            sbi, shard, &arena_start, &arena_end);
        if (arena_end <= arena_start || count > arena_end - arena_start)
            continue;
        spin_lock(&sbi->allocation_reservation_locks[shard]);
        read_lock(&sbi->bitmap_lock);
        hint = sbi->allocation_reservation_hints[shard];
        if (hint < arena_start || hint >= arena_end)
            hint = arena_start;

        for (sweep = 0; sweep < 2u; ++sweep) {
            u64 scan_start = sweep ? arena_start : hint;
            u64 scan_end = sweep ? hint : arena_end;
            u64 candidate;

            if (scan_end < scan_start || count > scan_end - scan_start)
                continue;
            for (candidate = scan_start;
                 candidate <= scan_end - count; ++candidate) {
                u64 block;

                if (!infilfs_parallel_range_free_locked(
                        sbi, candidate, count))
                    continue;
                for (block = candidate; block < candidate + count; ++block)
                    infilfs_parallel_bitmap_set(
                        sbi->allocation_reservations, block, true);
                sbi->allocation_reservation_hints[shard] =
                    candidate + count < arena_end ?
                        candidate + count : arena_start;
                read_unlock(&sbi->bitmap_lock);
                spin_unlock(&sbi->allocation_reservation_locks[shard]);
                reservation->start = candidate;
                reservation->count = count;
                reservation->shard = shard;
                reservation->active = true;
                infilfs_parallel_note_active(sbi);
                atomic64_inc(&sbi->allocation_reservation_successes);
                return 0;
            }
        }
        read_unlock(&sbi->bitmap_lock);
        spin_unlock(&sbi->allocation_reservation_locks[shard]);
    }
    atomic64_sub(count, &sbi->allocation_reserved_blocks);
    return -ENOSPC;
}

static void infilfs_parallel_release_reservation(
    struct super_block *sb, struct infilfs_parallel_reservation *reservation)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    u64 block;

    if (!reservation || !reservation->active || !sbi ||
        !sbi->allocation_reservations)
        return;
    spin_lock(&sbi->allocation_reservation_locks[reservation->shard]);
    for (block = reservation->start;
         block < reservation->start + reservation->count; ++block)
        infilfs_parallel_bitmap_set(
            sbi->allocation_reservations, block, false);
    spin_unlock(&sbi->allocation_reservation_locks[reservation->shard]);
    atomic64_sub(reservation->count, &sbi->allocation_reserved_blocks);
    atomic64_dec(&sbi->allocation_active_reservations);
    reservation->active = false;
}

static int infilfs_parallel_consume_reservation(
    struct infilfs_rw_tx *tx, struct infilfs_parallel_reservation *reservation,
    u64 count, u64 *start_out)
{
    int ret;

    if (!reservation || !reservation->active || reservation->count != count)
        return -ENOENT;
    ret = infilfs_parallel_tx_claim(
        tx, reservation->start, reservation->count, true);
    if (ret)
        return ret;
    *start_out = reservation->start;
    reservation->active = false;
    atomic64_dec(&tx->sbi->allocation_active_reservations);
    return 0;
}
