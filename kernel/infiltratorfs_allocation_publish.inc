// SPDX-License-Identifier: GPL-3.0-or-later

static size_t infilfs_rw_allocation_leaf_index(u64 block)
{
    return (size_t)(block / INFILFS_ALLOCATION_BITS_PER_LEAF);
}

static void infilfs_rw_allocation_mark_range(
    u8 *dirty, size_t leaf_count, u64 total, u64 start, u64 count)
{
    u64 end;
    size_t first, last, i;

    if (!dirty || !count || start >= total)
        return;
    end = start + count;
    if (end < start || end > total)
        end = total;
    first = infilfs_rw_allocation_leaf_index(start);
    last = infilfs_rw_allocation_leaf_index(end - 1u);
    if (first >= leaf_count)
        return;
    if (last >= leaf_count)
        last = leaf_count - 1u;
    for (i = first; i <= last; ++i)
        dirty[i] = 1;
}

static void infilfs_rw_allocation_mark_tx(
    const struct infilfs_rw_tx *tx, u8 *dirty, size_t leaf_count)
{
    size_t i;
    u64 total = infilfs_volume_blocks(tx->sbi);

    for (i = 0; i < tx->allocated_count; ++i)
        infilfs_rw_allocation_mark_range(
            dirty, leaf_count, total,
            tx->allocated[i].start, tx->allocated[i].count);
    for (i = 0; i < tx->deferred_count; ++i)
        infilfs_rw_allocation_mark_range(
            dirty, leaf_count, total,
            tx->deferred[i].start, tx->deferred[i].count);
}

static void infilfs_rw_allocation_close_dirty(
    const struct infilfs_allocation_layout *old,
    u8 *dirty_leaves, u8 *dirty_level1, u8 *dirty_level2, u64 total)
{
    size_t i;
    bool changed;
    size_t level1_base = 1u + old->level2_count;

    do {
        changed = false;

        for (i = 0; i < old->leaf_count; ++i) {
            size_t l1;

            if (!dirty_leaves[i])
                continue;
            l1 = i / INFILFS_ALLOCATION_TREE_FANOUT;
            if (l1 < old->level1_count && !dirty_level1[l1]) {
                dirty_level1[l1] = 1;
                changed = true;
            }
        }
        for (i = 0; i < old->level1_count; ++i) {
            size_t l2;

            if (!dirty_level1[i])
                continue;
            l2 = i / INFILFS_ALLOCATION_TREE_FANOUT;
            if (l2 < old->level2_count && !dirty_level2[l2]) {
                dirty_level2[l2] = 1;
                changed = true;
            }
        }

        {
            u64 block = old->branch_blocks[0];
            size_t owner = infilfs_rw_allocation_leaf_index(block);

            if (block < total && owner < old->leaf_count &&
                !dirty_leaves[owner]) {
                dirty_leaves[owner] = 1;
                changed = true;
            }
        }
        for (i = 0; i < old->level2_count; ++i) {
            u64 block;
            size_t owner;

            if (!dirty_level2[i])
                continue;
            block = old->branch_blocks[1u + i];
            owner = infilfs_rw_allocation_leaf_index(block);
            if (block < total && owner < old->leaf_count &&
                !dirty_leaves[owner]) {
                dirty_leaves[owner] = 1;
                changed = true;
            }
        }
        for (i = 0; i < old->level1_count; ++i) {
            u64 block;
            size_t owner;

            if (!dirty_level1[i])
                continue;
            block = old->branch_blocks[level1_base + i];
            owner = infilfs_rw_allocation_leaf_index(block);
            if (block < total && owner < old->leaf_count &&
                !dirty_leaves[owner]) {
                dirty_leaves[owner] = 1;
                changed = true;
            }
        }
        for (i = 0; i < old->leaf_count; ++i) {
            u64 block;
            size_t owner;

            if (!dirty_leaves[i])
                continue;
            block = old->leaf_blocks[i];
            if (block >= total)
                continue;
            owner = infilfs_rw_allocation_leaf_index(block);
            if (owner < old->leaf_count && !dirty_leaves[owner]) {
                dirty_leaves[owner] = 1;
                changed = true;
            }
        }
    } while (changed);
}

static size_t infilfs_rw_allocation_dirty_count(const u8 *dirty, size_t n)
{
    size_t i, count = 0;

    for (i = 0; i < n; ++i)
        count += dirty[i] != 0;
    return count;
}

int infilfs_rw_allocation_write_page(
    struct super_block *sb, u64 physical, const u8 magic[8],
    u64 generation, u64 logical, u32 level, u32 entries,
    const void *payload, u32 bytes)
{
    struct infilfs_allocation_page_disk *page;
    u8 *block;
    size_t checksum_off;
    __le64 crc;
    int ret;

    if (!physical || physical >= INFILFS_SB(sb)->device_blocks ||
        !entries || bytes > INFILFS_ALLOCATION_PAGE_DATA_SIZE)
        return -EINVAL;
    block = kzalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!block)
        return -ENOMEM;
    page = (struct infilfs_allocation_page_disk *)block;
    memcpy(page->magic, magic, 8);
    page->generation = cpu_to_le64(generation);
    page->logical_index = cpu_to_le64(logical);
    page->level = cpu_to_le32(level);
    page->entry_count = cpu_to_le32(entries);
    page->bytes_used = cpu_to_le32(bytes);
    page->checksum_type = cpu_to_le32(INFILFS_CHECKSUM_CRC64_ECMA);
    if (bytes)
        memcpy(page + 1, payload, bytes);
    checksum_off = offsetof(struct infilfs_allocation_page_disk, checksum);
    crc = cpu_to_le64(infilfs_rw_crc64_zeroed(
        block, INFILFS_DISK_BLOCK_SIZE, checksum_off,
        sizeof(page->checksum)));
    memcpy(page->checksum, &crc, sizeof(crc));
    ret = infilfs_rw_write_block(sb, physical, block);
    kfree(block);
    return ret;
}

static int infilfs_rw_allocation_map_publish(
    struct infilfs_rw_tx *tx,
    struct infilfs_allocation_layout *next_layout)
{
    struct infilfs_allocation_layout old = {0};
    struct super_block *sb = tx->sb;
    struct infilfs_sb_info *sbi = tx->sbi;
    u64 total = infilfs_volume_blocks(sbi);
    u64 *reserved = NULL;
    u64 *new_leaves = NULL;
    u64 *new_branches = NULL;
    u8 *dirty_leaves = NULL;
    u8 *dirty_level1 = NULL;
    u8 *dirty_level2 = NULL;
    __le64 *pointers = NULL;
    size_t reserved_count = 0;
    size_t max_reserved;
    size_t needed = 0;
    size_t i, r;
    int ret;

    if (!next_layout)
        return -EINVAL;
    ret = infilfs_allocation_cache_view(sbi, &tx->next_sb, &old);
    if (ret)
        return ret;
    if (old.leaf_count > SIZE_MAX - old.branch_count) {
        ret = -EOVERFLOW;
        goto out;
    }
    max_reserved = old.leaf_count + old.branch_count;
    dirty_leaves = kvzalloc(old.leaf_count, GFP_NOFS);
    dirty_level1 = kvzalloc(old.level1_count, GFP_NOFS);
    dirty_level2 = kvzalloc(old.level2_count, GFP_NOFS);
    reserved = kvmalloc_array(max_reserved, sizeof(*reserved), GFP_NOFS);
    new_leaves = kvmalloc_array(old.leaf_count, sizeof(*new_leaves), GFP_NOFS);
    new_branches = kvmalloc_array(old.branch_count, sizeof(*new_branches),
                                  GFP_NOFS);
    pointers = kvmalloc_array(INFILFS_ALLOCATION_TREE_FANOUT,
                              sizeof(*pointers), GFP_NOFS);
    if (!dirty_leaves || !dirty_level1 || !dirty_level2 ||
        !reserved || !new_leaves || !new_branches || !pointers) {
        ret = -ENOMEM;
        goto out;
    }

    infilfs_rw_allocation_mark_tx(tx, dirty_leaves, old.leaf_count);
    infilfs_rw_allocation_close_dirty(
        &old, dirty_leaves, dirty_level1, dirty_level2, total);

    for (;;) {
        size_t dirty_leaf_count =
            infilfs_rw_allocation_dirty_count(dirty_leaves, old.leaf_count);
        size_t dirty_l1_count =
            infilfs_rw_allocation_dirty_count(dirty_level1, old.level1_count);
        size_t dirty_l2_count =
            infilfs_rw_allocation_dirty_count(dirty_level2, old.level2_count);
        size_t before;
        size_t after;

        if (dirty_leaf_count > SIZE_MAX - dirty_l1_count ||
            dirty_leaf_count + dirty_l1_count > SIZE_MAX - dirty_l2_count ||
            dirty_leaf_count + dirty_l1_count + dirty_l2_count >
                SIZE_MAX - 1u) {
            ret = -EOVERFLOW;
            goto out;
        }
        needed = dirty_leaf_count + dirty_l1_count + dirty_l2_count + 1u;
        if (needed > max_reserved) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        while (reserved_count < needed) {
            ret = infilfs_rw_tx_alloc(tx, 1u, &reserved[reserved_count]);
            if (ret)
                goto out;
            ++reserved_count;
        }

        before = needed;
        infilfs_rw_allocation_mark_tx(tx, dirty_leaves, old.leaf_count);
        infilfs_rw_allocation_close_dirty(
            &old, dirty_leaves, dirty_level1, dirty_level2, total);

        dirty_leaf_count =
            infilfs_rw_allocation_dirty_count(dirty_leaves, old.leaf_count);
        dirty_l1_count =
            infilfs_rw_allocation_dirty_count(dirty_level1, old.level1_count);
        dirty_l2_count =
            infilfs_rw_allocation_dirty_count(dirty_level2, old.level2_count);
        if (dirty_leaf_count > SIZE_MAX - dirty_l1_count ||
            dirty_leaf_count + dirty_l1_count > SIZE_MAX - dirty_l2_count ||
            dirty_leaf_count + dirty_l1_count + dirty_l2_count >
                SIZE_MAX - 1u) {
            ret = -EOVERFLOW;
            goto out;
        }
        after = dirty_leaf_count + dirty_l1_count + dirty_l2_count + 1u;
        if (after == before && reserved_count >= after) {
            needed = after;
            break;
        }
    }

    memcpy(new_leaves, old.leaf_blocks,
           old.leaf_count * sizeof(*new_leaves));
    memcpy(new_branches, old.branch_blocks,
           old.branch_count * sizeof(*new_branches));

    r = 0;
    for (i = 0; i < old.leaf_count; ++i)
        if (dirty_leaves[i])
            new_leaves[i] = reserved[r++];

    new_branches[0] = reserved[r++];
    for (i = 0; i < old.level2_count; ++i)
        if (dirty_level2[i])
            new_branches[1u + i] = reserved[r++];

    {
        size_t level1_base = 1u + old.level2_count;

        for (i = 0; i < old.level1_count; ++i)
            if (dirty_level1[i])
                new_branches[level1_base + i] = reserved[r++];

        if (r != needed || r > reserved_count) {
            ret = -EFSCORRUPTED;
            goto out;
        }

        ret = infilfs_rw_tx_defer_free(tx, old.branch_blocks[0], 1u);
        if (ret)
            goto out;
        for (i = 0; i < old.level2_count; ++i) {
            if (!dirty_level2[i])
                continue;
            ret = infilfs_rw_tx_defer_free(
                tx, old.branch_blocks[1u + i], 1u);
            if (ret)
                goto out;
        }
        for (i = 0; i < old.level1_count; ++i) {
            if (!dirty_level1[i])
                continue;
            ret = infilfs_rw_tx_defer_free(
                tx, old.branch_blocks[level1_base + i], 1u);
            if (ret)
                goto out;
        }
        for (i = 0; i < old.leaf_count; ++i) {
            if (!dirty_leaves[i])
                continue;
            ret = infilfs_rw_tx_defer_free(tx, old.leaf_blocks[i], 1u);
            if (ret)
                goto out;
        }

        ret = infilfs_rw_tx_apply_deferred(tx);
        if (ret)
            goto out;

        tx->next_sb.allocation_root_block = cpu_to_le64(new_branches[0]);
        tx->next_sb.allocation_leaf_count = cpu_to_le64(old.leaf_count);
        tx->next_sb.generation = cpu_to_le64(tx->generation);
        tx->next_sb.free_blocks = cpu_to_le64(tx->free_blocks);

        for (i = 0; i < old.leaf_count; ++i) {
            u64 first, remaining;
            u32 valid, bytes;
            size_t offset;

            if (!dirty_leaves[i])
                continue;
            first = (u64)i * INFILFS_ALLOCATION_BITS_PER_LEAF;
            remaining = total - first;
            valid = (u32)min_t(u64,
                INFILFS_ALLOCATION_BITS_PER_LEAF, remaining);
            bytes = DIV_ROUND_UP(valid, 8u);
            offset = i * (size_t)INFILFS_ALLOCATION_PAGE_DATA_SIZE;
            if (offset > tx->bitmap_bytes ||
                bytes > tx->bitmap_bytes - offset) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            ret = infilfs_rw_allocation_write_page(
                sb, new_leaves[i], (const u8 *)"INFSAL01",
                tx->generation, i, 0u, valid, tx->bitmap + offset, bytes);
            if (ret)
                goto out;
        }

        for (i = 0; i < old.level1_count; ++i) {
            size_t first;
            u32 entries;
            u32 j;

            if (!dirty_level1[i])
                continue;
            first = i * INFILFS_ALLOCATION_TREE_FANOUT;
            entries = (u32)min_t(size_t,
                INFILFS_ALLOCATION_TREE_FANOUT,
                old.leaf_count - first);
            for (j = 0; j < entries; ++j)
                pointers[j] = cpu_to_le64(new_leaves[first + j]);
            ret = infilfs_rw_allocation_write_page(
                sb, new_branches[level1_base + i],
                (const u8 *)"INFSAB01", tx->generation,
                i, 1u, entries, pointers, entries * sizeof(__le64));
            if (ret)
                goto out;
        }

        for (i = 0; i < old.level2_count; ++i) {
            size_t first;
            u32 entries;
            u32 j;

            if (!dirty_level2[i])
                continue;
            first = i * INFILFS_ALLOCATION_TREE_FANOUT;
            entries = (u32)min_t(size_t,
                INFILFS_ALLOCATION_TREE_FANOUT,
                old.level1_count - first);
            for (j = 0; j < entries; ++j)
                pointers[j] = cpu_to_le64(
                    new_branches[level1_base + first + j]);
            ret = infilfs_rw_allocation_write_page(
                sb, new_branches[1u + i], (const u8 *)"INFSAB01",
                tx->generation, i, 2u, entries, pointers,
                entries * sizeof(__le64));
            if (ret)
                goto out;
        }
    }

    {
        for (i = 0; i < old.level2_count; ++i)
            pointers[i] = cpu_to_le64(new_branches[1u + i]);
        ret = infilfs_rw_allocation_write_page(
            sb, new_branches[0], (const u8 *)"INFSAB01",
            tx->generation, 0u, INFILFS_ALLOCATION_TREE_ROOT_LEVEL,
            (u32)old.level2_count, pointers,
            (u32)old.level2_count * sizeof(__le64));
    }

    if (!ret) {
        next_layout->leaf_blocks = new_leaves;
        next_layout->branch_blocks = new_branches;
        next_layout->leaf_count = old.leaf_count;
        next_layout->branch_count = old.branch_count;
        next_layout->level1_count = old.level1_count;
        next_layout->level2_count = old.level2_count;
        new_leaves = NULL;
        new_branches = NULL;
    }

out:
    kvfree(pointers);
    kvfree(new_branches);
    kvfree(new_leaves);
    kvfree(reserved);
    kvfree(dirty_level2);
    kvfree(dirty_level1);
    kvfree(dirty_leaves);
    return ret;
}
