// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"
/*
 * Native mounted filesystem resize.
 *
 * Format 0.17 keeps physical backing capacity separate from committed
 * filesystem geometry. Online grow and bounded shrink therefore rebuild only
 * allocation-tree/checkpoint geometry. Shrink fails closed if any ordinary
 * live allocation would cross the requested boundary.
 */

#define INFILFS_NATIVE_RESIZE_MIN_BYTES (16ULL * 1024ULL * 1024ULL)

static bool infilfs_resize_old_allocation_page(
    const struct infilfs_sb_info *sbi, u64 block)
{
    size_t i;

    for (i = 0; i < sbi->allocation_leaf_count; ++i)
        if (sbi->allocation_leaf_blocks[i] == block)
            return true;
    for (i = 0; i < sbi->allocation_branch_count; ++i)
        if (sbi->allocation_branch_blocks[i] == block)
            return true;
    return false;
}

static bool infilfs_resize_old_checkpoint(
    const struct infilfs_sb_info *sbi, u64 block)
{
    unsigned int i;

    for (i = 0; i < INFILFS_CHECKPOINT_COUNT; ++i)
        if (le64_to_cpu(sbi->disk.checkpoint_block[i]) == block)
            return true;
    return false;
}

static u64 infilfs_resize_pick_free_near(
    u8 *bitmap, u64 total, u64 preferred)
{
    u64 distance;

    if (!bitmap || total <= 1u)
        return 0;
    if (preferred >= total)
        preferred = total - 1u;

    for (distance = 0; distance < total; ++distance) {
        if (preferred >= distance) {
            u64 block = preferred - distance;

            if (block && !infilfs_rw_bitmap_get(bitmap, block)) {
                infilfs_rw_bitmap_set(bitmap, block, true);
                return block;
            }
        }
        if (distance && preferred <= U64_MAX - distance) {
            u64 block = preferred + distance;

            if (block < total && block &&
                !infilfs_rw_bitmap_get(bitmap, block)) {
                infilfs_rw_bitmap_set(bitmap, block, true);
                return block;
            }
        }
    }
    return 0;
}

static u64 infilfs_resize_pick_free_reverse(
    u8 *bitmap, u64 total, u64 *cursor)
{
    u64 block;

    if (!bitmap || !cursor || total <= 1u)
        return 0;
    block = *cursor < total ? *cursor : total - 1u;
    while (block) {
        if (!infilfs_rw_bitmap_get(bitmap, block)) {
            infilfs_rw_bitmap_set(bitmap, block, true);
            *cursor = block - 1u;
            return block;
        }
        --block;
    }
    return 0;
}

static u64 infilfs_resize_count_free(const u8 *bitmap, u64 total)
{
    u64 block;
    u64 count = 0;

    for (block = 0; block < total; ++block)
        if (!infilfs_rw_bitmap_get(bitmap, block))
            ++count;
    return count;
}

static int infilfs_resize_allocate_layout(
    u8 *bitmap, u64 total, struct infilfs_allocation_layout *layout)
{
    size_t leaves = 0, level1 = 0, level2 = 0, branches = 0;
    u64 cursor;
    size_t i;
    int ret;

    ret = infilfs_allocation_counts(
        total, &leaves, &level1, &level2, &branches);
    if (ret)
        return ret;
    if (!leaves || !branches ||
        leaves > SIZE_MAX / sizeof(u64) ||
        branches > SIZE_MAX / sizeof(u64))
        return -EOVERFLOW;

    memset(layout, 0, sizeof(*layout));
    layout->leaf_blocks = kvmalloc_array(leaves, sizeof(u64), GFP_NOFS);
    layout->branch_blocks = kvmalloc_array(branches, sizeof(u64), GFP_NOFS);
    if (!layout->leaf_blocks || !layout->branch_blocks) {
        infilfs_allocation_layout_destroy(layout);
        return -ENOMEM;
    }
    layout->leaf_count = leaves;
    layout->branch_count = branches;
    layout->level1_count = level1;
    layout->level2_count = level2;

    cursor = total - 1u;
    for (i = 0; i < leaves; ++i) {
        u64 block = infilfs_resize_pick_free_reverse(bitmap, total, &cursor);

        if (!block) {
            infilfs_allocation_layout_destroy(layout);
            return -ENOSPC;
        }
        layout->leaf_blocks[i] = block;
    }
    for (i = 0; i < branches; ++i) {
        u64 block = infilfs_resize_pick_free_reverse(bitmap, total, &cursor);

        if (!block) {
            infilfs_allocation_layout_destroy(layout);
            return -ENOSPC;
        }
        layout->branch_blocks[i] = block;
    }
    return 0;
}

static int infilfs_resize_write_layout(
    struct super_block *sb, const u8 *bitmap, size_t bitmap_bytes,
    u64 total, u64 generation,
    const struct infilfs_allocation_layout *layout)
{
    const size_t level1_base = 1u + layout->level2_count;
    __le64 *pointers;
    size_t i;
    int ret = 0;

    pointers = kvmalloc_array(INFILFS_ALLOCATION_TREE_FANOUT,
                              sizeof(*pointers), GFP_NOFS);
    if (!pointers)
        return -ENOMEM;

    for (i = 0; i < layout->leaf_count; ++i) {
        u64 first_bit = (u64)i * INFILFS_ALLOCATION_BITS_PER_LEAF;
        u64 remaining = total - first_bit;
        u32 valid = (u32)min_t(u64,
            INFILFS_ALLOCATION_BITS_PER_LEAF, remaining);
        u32 bytes = DIV_ROUND_UP(valid, 8u);
        size_t offset = i * (size_t)INFILFS_ALLOCATION_PAGE_DATA_SIZE;

        if (offset > bitmap_bytes || bytes > bitmap_bytes - offset) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        ret = infilfs_rw_allocation_write_page(
            sb, layout->leaf_blocks[i], (const u8 *)"INFSAL01",
            generation, i, 0u, valid, bitmap + offset, bytes);
        if (ret)
            goto out;
    }

    for (i = 0; i < layout->level1_count; ++i) {
        size_t first = i * INFILFS_ALLOCATION_TREE_FANOUT;
        u32 entries = (u32)min_t(size_t,
            INFILFS_ALLOCATION_TREE_FANOUT, layout->leaf_count - first);
        u32 j;

        for (j = 0; j < entries; ++j)
            pointers[j] = cpu_to_le64(layout->leaf_blocks[first + j]);
        ret = infilfs_rw_allocation_write_page(
            sb, layout->branch_blocks[level1_base + i],
            (const u8 *)"INFSAB01", generation, i, 1u,
            entries, pointers, entries * sizeof(__le64));
        if (ret)
            goto out;
    }

    for (i = 0; i < layout->level2_count; ++i) {
        size_t first = i * INFILFS_ALLOCATION_TREE_FANOUT;
        u32 entries = (u32)min_t(size_t,
            INFILFS_ALLOCATION_TREE_FANOUT,
            layout->level1_count - first);
        u32 j;

        for (j = 0; j < entries; ++j)
            pointers[j] = cpu_to_le64(
                layout->branch_blocks[level1_base + first + j]);
        ret = infilfs_rw_allocation_write_page(
            sb, layout->branch_blocks[1u + i],
            (const u8 *)"INFSAB01", generation, i, 2u,
            entries, pointers, entries * sizeof(__le64));
        if (ret)
            goto out;
    }

    for (i = 0; i < layout->level2_count; ++i)
        pointers[i] = cpu_to_le64(layout->branch_blocks[1u + i]);
    ret = infilfs_rw_allocation_write_page(
        sb, layout->branch_blocks[0], (const u8 *)"INFSAB01",
        generation, 0u, INFILFS_ALLOCATION_TREE_ROOT_LEVEL,
        (u32)layout->level2_count, pointers,
        (u32)layout->level2_count * sizeof(__le64));
out:
    kvfree(pointers);
    return ret;
}

static int infilfs_resize_wait_for_reservations(struct infilfs_sb_info *sbi)
{
    unsigned int attempt;

    for (attempt = 0; attempt < 5000u; ++attempt) {
        if (atomic64_read(&sbi->allocation_active_reservations) == 0)
            return 0;
        msleep(1);
    }
    return -EBUSY;
}

static int infilfs_native_resize_locked(
    struct super_block *sb, u64 new_size_bytes,
    struct infilfs_resize_request *request)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct infilfs_allocation_layout next_layout = {0};
    struct infilfs_superblock_disk next_sb;
    unsigned long *new_reservations = NULL;
    unsigned long *old_reservations;
    u8 *next_bitmap = NULL;
    u8 *old_bitmap;
    u8 *sb_block = NULL;
    size_t next_bitmap_bytes = 0;
    size_t new_reservation_bytes = 0;
    u64 checkpoint[INFILFS_CHECKPOINT_COUNT];
    u64 old_total = infilfs_volume_blocks(sbi);
    u64 new_total;
    u64 generation;
    u64 common;
    u64 bit;
    u64 words;
    u32 snapshots = 0;
    u32 slot;
    int ret;
    int replica_error = 0;

    if ((new_size_bytes & (INFILFS_DISK_BLOCK_SIZE - 1u)) != 0 ||
        new_size_bytes < INFILFS_NATIVE_RESIZE_MIN_BYTES)
        return -EINVAL;
    new_total = new_size_bytes >> INFILFS_DISK_BLOCK_SHIFT;
    if (new_total > sbi->device_blocks)
        return -ENOSPC;
    ret = infilfs_allocation_runtime_bytes(new_total, &next_bitmap_bytes);
    if (ret)
        return ret;
    if (new_total == old_total) {
        request->new_size_bytes = new_size_bytes;
        return 0;
    }

    ret = infilfs_rw_snapshot_count(sb, &snapshots);
    if (ret)
        return ret;
    if (snapshots)
        return -EBUSY;

    if (new_total < old_total) {
        for (bit = new_total; bit < old_total; ++bit) {
            if (!infilfs_rw_bitmap_get(sbi->bitmap, bit))
                continue;
            if (infilfs_resize_old_allocation_page(sbi, bit) ||
                infilfs_resize_old_checkpoint(sbi, bit))
                continue;
            return -EBUSY;
        }
    }

    next_bitmap = kvzalloc(next_bitmap_bytes, GFP_NOFS);
    if (!next_bitmap)
        return -ENOMEM;
    common = min_t(u64, old_total, new_total);
    for (bit = 0; bit < common; ++bit)
        if (infilfs_rw_bitmap_get(sbi->bitmap, bit))
            infilfs_rw_bitmap_set(next_bitmap, bit, true);
    for (bit = new_total; bit < (u64)next_bitmap_bytes * 8u; ++bit)
        infilfs_rw_bitmap_set(next_bitmap, bit, true);

    checkpoint[0] = 0;
    checkpoint[1] = le64_to_cpu(sbi->disk.checkpoint_block[1]);
    checkpoint[2] = le64_to_cpu(sbi->disk.checkpoint_block[2]);
    infilfs_rw_bitmap_set(next_bitmap, 0, true);

    if (checkpoint[1] >= new_total) {
        checkpoint[1] = infilfs_resize_pick_free_near(
            next_bitmap, new_total, new_total / 2u);
        if (!checkpoint[1]) {
            ret = -ENOSPC;
            goto out;
        }
    } else {
        infilfs_rw_bitmap_set(next_bitmap, checkpoint[1], true);
    }
    if (checkpoint[2] >= new_total || checkpoint[2] == checkpoint[1]) {
        checkpoint[2] = infilfs_resize_pick_free_near(
            next_bitmap, new_total, new_total - 1u);
        if (!checkpoint[2]) {
            ret = -ENOSPC;
            goto out;
        }
    } else {
        infilfs_rw_bitmap_set(next_bitmap, checkpoint[2], true);
    }

    ret = infilfs_resize_allocate_layout(
        next_bitmap, new_total, &next_layout);
    if (ret)
        goto out;

    for (bit = 0; bit < sbi->allocation_leaf_count; ++bit) {
        u64 block = sbi->allocation_leaf_blocks[bit];

        if (block < new_total)
            infilfs_rw_bitmap_set(next_bitmap, block, false);
    }
    for (bit = 0; bit < sbi->allocation_branch_count; ++bit) {
        u64 block = sbi->allocation_branch_blocks[bit];

        if (block < new_total)
            infilfs_rw_bitmap_set(next_bitmap, block, false);
    }
    for (slot = 1; slot < INFILFS_CHECKPOINT_COUNT; ++slot) {
        u64 old = le64_to_cpu(sbi->disk.checkpoint_block[slot]);

        if (old < new_total &&
            old != checkpoint[1] && old != checkpoint[2])
            infilfs_rw_bitmap_set(next_bitmap, old, false);
    }

    generation = le64_to_cpu(sbi->disk.generation);
    if (generation == U64_MAX) {
        ret = -EOVERFLOW;
        goto out;
    }
    ++generation;

    words = DIV_ROUND_UP_ULL(new_total, BITS_PER_LONG);
    if (words > SIZE_MAX / sizeof(unsigned long)) {
        ret = -EOVERFLOW;
        goto out;
    }
    new_reservation_bytes = (size_t)words * sizeof(unsigned long);
    new_reservations = kvzalloc(new_reservation_bytes, GFP_NOFS);
    if (!new_reservations) {
        ret = -ENOMEM;
        goto out;
    }

    ret = infilfs_resize_write_layout(
        sb, next_bitmap, next_bitmap_bytes,
        new_total, generation, &next_layout);
    if (ret)
        goto out;
    ret = sync_blockdev(sb->s_bdev);
    if (ret)
        goto out;

    next_sb = sbi->disk;
    next_sb.total_blocks = cpu_to_le64(new_total);
    next_sb.free_blocks =
        cpu_to_le64(infilfs_resize_count_free(next_bitmap, new_total));
    next_sb.allocation_root_block =
        cpu_to_le64(next_layout.branch_blocks[0]);
    next_sb.allocation_leaf_count = cpu_to_le64(next_layout.leaf_count);
    next_sb.generation = cpu_to_le64(generation);
    for (slot = 0; slot < INFILFS_CHECKPOINT_COUNT; ++slot)
        next_sb.checkpoint_block[slot] = cpu_to_le64(checkpoint[slot]);

    sb_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!sb_block) {
        ret = -ENOMEM;
        goto out;
    }
    infilfs_rw_encode_superblock(&next_sb, sb_block);

    /*
     * Block zero is the resize commit point. All replacement allocation pages
     * are already durable and old metadata was never overwritten, so recovery
     * sees either the complete old geometry or the complete new geometry.
     */
    ret = infilfs_rw_write_block(sb, 0, sb_block);
    if (ret)
        goto out;
    ret = sync_blockdev(sb->s_bdev);
    if (ret) {
        sbi->write_poisoned = true;
        sbi->checkpoint_repair_needed = true;
        goto out;
    }

    old_bitmap = sbi->bitmap;
    old_reservations = sbi->allocation_reservations;
    sbi->bitmap = next_bitmap;
    sbi->bitmap_bytes = next_bitmap_bytes;
    next_bitmap = NULL;
    sbi->disk = next_sb;
    infilfs_allocation_cache_replace(sbi, &next_layout);
    write_lock(&sbi->bitmap_lock);
    sbi->visible_bitmap = sbi->bitmap;
    sbi->visible_bitmap_bytes = sbi->bitmap_bytes;
    write_unlock(&sbi->bitmap_lock);

    sbi->allocation_reservations = new_reservations;
    sbi->allocation_reservation_bytes = new_reservation_bytes;
    new_reservations = NULL;
    atomic64_set(&sbi->allocation_reserved_blocks, 0);
    atomic64_set(&sbi->allocation_active_reservations, 0);
    atomic64_set(&sbi->allocation_reservation_steer, 0);
    for (slot = 0; slot < INFILFS_ALLOCATION_RESERVATION_SHARDS; ++slot) {
        u64 start, end;

        infilfs_parallel_shard_bounds(sbi, slot, &start, &end);
        sbi->allocation_reservation_hints[slot] = start;
    }
    sbi->data_alloc_hint = 1u;
    sbi->metadata_alloc_hint = new_total - 1u;

    kvfree(old_bitmap);
    kvfree(old_reservations);

    for (slot = 1; slot < INFILFS_CHECKPOINT_COUNT; ++slot) {
        int one = infilfs_rw_write_block(sb, checkpoint[slot], sb_block);

        if (one && !replica_error)
            replica_error = one;
    }
    ret = sync_blockdev(sb->s_bdev);
    if (ret && !replica_error)
        replica_error = ret;
    sbi->checkpoint_repair_needed = replica_error != 0;
    if (replica_error) {
        sbi->write_poisoned = true;
        pr_warn("InfiltratorFS: resize committed but checkpoint replica repair is required; further writes disabled until remount\n");
    }

    request->new_size_bytes = new_total << INFILFS_DISK_BLOCK_SHIFT;
    pr_info("InfiltratorFS: online resize %llu -> %llu bytes generation=%llu%s\n",
            (unsigned long long)request->old_size_bytes,
            (unsigned long long)request->new_size_bytes,
            (unsigned long long)generation,
            replica_error ? " (replica repair required)" : "");
    ret = 0;
out:
    kfree(sb_block);
    kvfree(new_reservations);
    kvfree(next_bitmap);
    infilfs_allocation_layout_destroy(&next_layout);
    return ret;
}

int infilfs_native_resize_volume(
    struct super_block *sb, struct infilfs_resize_request *request)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    u64 requested;
    int ret;

    if (!sbi || !request)
        return -EINVAL;
    if (sb_rdonly(sb))
        return -EROFS;
    if (sbi->write_poisoned)
        return -EIO;
    if (request->flags & ~INFILFS_RESIZE_TO_DEVICE_MAX)
        return -EINVAL;
    if ((request->flags & INFILFS_RESIZE_TO_DEVICE_MAX) &&
        request->size_bytes)
        return -EINVAL;

    request->old_size_bytes =
        infilfs_volume_blocks(sbi) << INFILFS_DISK_BLOCK_SHIFT;
    request->new_size_bytes = request->old_size_bytes;
    requested = (request->flags & INFILFS_RESIZE_TO_DEVICE_MAX) ?
        sbi->device_blocks << INFILFS_DISK_BLOCK_SHIFT :
        request->size_bytes;

    mutex_lock(&sbi->resize_lock);
    WRITE_ONCE(sbi->resize_active, true);

    /*
     * Stop new transactions/reservations, then drain anything that began just
     * before the gate. Do not hold write_lock while waiting: a reservation may
     * still need that lock to finish its already-started transaction.
     */
    ret = infilfs_native_pending_flush_sb(sb);
    if (!ret)
        ret = infilfs_resize_wait_for_reservations(sbi);
    if (!ret) {
        mutex_lock(&sbi->write_lock);
        ret = infilfs_native_resize_locked(sb, requested, request);
        mutex_unlock(&sbi->write_lock);
    }

    WRITE_ONCE(sbi->resize_active, false);
    mutex_unlock(&sbi->resize_lock);
    return ret;
}
