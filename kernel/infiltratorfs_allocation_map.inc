// SPDX-License-Identifier: GPL-3.0-or-later

struct infilfs_allocation_layout {
    u64 *leaf_blocks;
    u64 *branch_blocks;
    size_t leaf_count;
    size_t branch_count;
    size_t level1_count;
    size_t level2_count;
};

static void infilfs_allocation_layout_destroy(
    struct infilfs_allocation_layout *layout)
{
    if (!layout)
        return;
    kvfree(layout->leaf_blocks);
    kvfree(layout->branch_blocks);
    memset(layout, 0, sizeof(*layout));
}

static int infilfs_allocation_counts(
    u64 total, size_t *leaves_out, size_t *level1_out,
    size_t *level2_out, size_t *branches_out)
{
    u64 leaves, level1, level2, branches;

    if (!total || total > INFILFS_ALLOCATION_TREE_MAX_BLOCKS)
        return -EFBIG;
    leaves = DIV_ROUND_UP_ULL(total, INFILFS_ALLOCATION_BITS_PER_LEAF);
    level1 = DIV_ROUND_UP_ULL(leaves, INFILFS_ALLOCATION_TREE_FANOUT);
    level2 = DIV_ROUND_UP_ULL(level1, INFILFS_ALLOCATION_TREE_FANOUT);
    branches = 1 + level1 + level2;
    if (!leaves || !level1 || !level2 ||
        leaves > SIZE_MAX || level1 > SIZE_MAX ||
        level2 > SIZE_MAX || branches > SIZE_MAX)
        return -EOVERFLOW;
    if (leaves_out)
        *leaves_out = (size_t)leaves;
    if (level1_out)
        *level1_out = (size_t)level1;
    if (level2_out)
        *level2_out = (size_t)level2;
    if (branches_out)
        *branches_out = (size_t)branches;
    return 0;
}

static int infilfs_allocation_runtime_bytes(u64 total, size_t *bytes_out)
{
    u64 raw, blocks;

    if (!bytes_out || total > U64_MAX - 7)
        return -EINVAL;
    raw = DIV_ROUND_UP_ULL(total, 8);
    blocks = DIV_ROUND_UP_ULL(raw, INFILFS_DISK_BLOCK_SIZE);
    if (!blocks || blocks > SIZE_MAX / INFILFS_DISK_BLOCK_SIZE)
        return -EOVERFLOW;
    *bytes_out = (size_t)blocks * INFILFS_DISK_BLOCK_SIZE;
    return 0;
}

static void infilfs_allocation_bitmap_set(u8 *bitmap, u64 block, bool used)
{
    u8 mask = (u8)(1u << (block & 7u));

    if (used)
        bitmap[block >> 3] |= mask;
    else
        bitmap[block >> 3] &= (u8)~mask;
}

static bool infilfs_allocation_bitmap_get(const u8 *bitmap, u64 block)
{
    return (bitmap[block >> 3] & (u8)(1u << (block & 7u))) != 0;
}

static int infilfs_allocation_u64_compare(const void *a, const void *b)
{
    u64 av = *(const u64 *)a;
    u64 bv = *(const u64 *)b;

    return av > bv ? 1 : av < bv ? -1 : 0;
}

static bool infilfs_allocation_page_valid(
    const u8 block[INFILFS_DISK_BLOCK_SIZE], const u8 magic[8],
    u64 max_generation, u64 logical_index, u32 level, u32 expected_entries)
{
    const struct infilfs_allocation_page_disk *page =
        (const struct infilfs_allocation_page_disk *)block;
    u64 generation = le64_to_cpu(page->generation);
    u32 entries = le32_to_cpu(page->entry_count);
    u32 bytes = le32_to_cpu(page->bytes_used);
    bool leaf = level == 0;
    size_t off = offsetof(struct infilfs_allocation_page_disk, checksum);

    if (memcmp(page->magic, magic, 8) ||
        !generation || generation > max_generation ||
        le64_to_cpu(page->logical_index) != logical_index ||
        le32_to_cpu(page->level) != level ||
        entries != expected_entries ||
        le32_to_cpu(page->checksum_type) != INFILFS_CHECKSUM_CRC64_ECMA)
        return false;
    if (leaf) {
        if (!entries || entries > INFILFS_ALLOCATION_BITS_PER_LEAF ||
            bytes != DIV_ROUND_UP(entries, 8u))
            return false;
    } else if (!entries || entries > INFILFS_ALLOCATION_TREE_FANOUT ||
               bytes != entries * sizeof(__le64)) {
        return false;
    }
    if (bytes > INFILFS_ALLOCATION_PAGE_DATA_SIZE ||
        memchr_inv(block + sizeof(*page) + bytes, 0,
                   INFILFS_DISK_BLOCK_SIZE - sizeof(*page) - bytes))
        return false;
    return infilfs_crc64_block_valid(
        block, off, sizeof(page->checksum));
}

static int infilfs_allocation_read_page(
    struct super_block *sb, u64 physical, const u8 magic[8],
    u64 generation, u64 logical, u32 level, u32 entries,
    u8 block[INFILFS_DISK_BLOCK_SIZE])
{
    if (!physical || physical >= INFILFS_SB(sb)->device_blocks)
        return -EFSCORRUPTED;
    int ret = infilfs_read_block(sb, physical, block);
    if (ret)
        return ret;
    return infilfs_allocation_page_valid(
        block, magic, generation, logical, level, entries) ?
        0 : -EFSCORRUPTED;
}

static int infilfs_allocation_map_load(
    struct super_block *sb, const struct infilfs_superblock_disk *disk,
    u8 **bitmap_out, size_t *bitmap_bytes_out,
    struct infilfs_allocation_layout *layout_out)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct infilfs_allocation_layout local = {0};
    struct infilfs_allocation_layout *layout =
        layout_out ? layout_out : &local;
    u64 total = le64_to_cpu(disk->total_blocks);
    u64 generation = le64_to_cpu(disk->generation);
    u64 root = le64_to_cpu(disk->allocation_root_block);
    size_t leaves = 0, level1 = 0, level2 = 0, branches = 0;
    size_t bitmap_bytes = 0;
    u8 *bitmap = NULL;
    u8 *page = NULL;
    u64 *all_pages = NULL;
    size_t branch_pos = 1;
    size_t global_l1 = 0;
    size_t global_leaf = 0;
    int ret;

    ret = infilfs_allocation_counts(
        total, &leaves, &level1, &level2, &branches);
    if (ret)
        return ret;
    if (!(le64_to_cpu(disk->incompat_flags) &
          INFILFS_INCOMPAT_ALLOCATION_TREE) ||
        le64_to_cpu(disk->allocation_leaf_count) != leaves ||
        !root || root >= total || !generation)
        return -EFSCORRUPTED;

    infilfs_allocation_layout_destroy(layout);
    layout->leaf_blocks = kvmalloc_array(leaves, sizeof(u64), GFP_KERNEL);
    layout->branch_blocks = kvmalloc_array(branches, sizeof(u64), GFP_KERNEL);
    page = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!layout->leaf_blocks || !layout->branch_blocks || !page) {
        ret = -ENOMEM;
        goto fail;
    }
    memset(layout->leaf_blocks, 0, leaves * sizeof(u64));
    memset(layout->branch_blocks, 0, branches * sizeof(u64));
    layout->leaf_count = leaves;
    layout->branch_count = branches;
    layout->level1_count = level1;
    layout->level2_count = level2;
    layout->branch_blocks[0] = root;

    if (bitmap_out) {
        ret = infilfs_allocation_runtime_bytes(total, &bitmap_bytes);
        if (ret)
            goto fail;
        bitmap = kvzalloc(bitmap_bytes, GFP_KERNEL);
        if (!bitmap) {
            ret = -ENOMEM;
            goto fail;
        }
    }

    ret = infilfs_allocation_read_page(
        sb, root, (const u8 *)"INFSAB01", generation, 0,
        INFILFS_ALLOCATION_TREE_ROOT_LEVEL, (u32)level2, page);
    if (ret)
        goto fail;

    {
        const __le64 *root_ptr =
            (const __le64 *)(page + sizeof(struct infilfs_allocation_page_disk));
        __le64 l2_ptrs[INFILFS_ALLOCATION_TREE_FANOUT];

        memcpy(l2_ptrs, root_ptr, level2 * sizeof(__le64));
        for (size_t i = 0; i < level2; ++i) {
            u64 l2_block = le64_to_cpu(l2_ptrs[i]);
            u32 l2_entries = min_t(size_t,
                INFILFS_ALLOCATION_TREE_FANOUT, level1 - global_l1);
            __le64 l1_ptrs[INFILFS_ALLOCATION_TREE_FANOUT];

            if (branch_pos >= branches) {
                ret = -EFSCORRUPTED;
                goto fail;
            }
            layout->branch_blocks[branch_pos++] = l2_block;
            ret = infilfs_allocation_read_page(
                sb, l2_block, (const u8 *)"INFSAB01", generation,
                i, 2u, l2_entries, page);
            if (ret)
                goto fail;
            memcpy(l1_ptrs,
                   page + sizeof(struct infilfs_allocation_page_disk),
                   (size_t)l2_entries * sizeof(__le64));

            for (u32 j = 0; j < l2_entries; ++j, ++global_l1) {
                u64 l1_block = le64_to_cpu(l1_ptrs[j]);
                u32 l1_entries = min_t(size_t,
                    INFILFS_ALLOCATION_TREE_FANOUT,
                    leaves - global_leaf);
                __le64 leaf_ptrs[INFILFS_ALLOCATION_TREE_FANOUT];

                if (branch_pos >= branches) {
                    ret = -EFSCORRUPTED;
                    goto fail;
                }
                layout->branch_blocks[branch_pos++] = l1_block;
                ret = infilfs_allocation_read_page(
                    sb, l1_block, (const u8 *)"INFSAB01", generation,
                    global_l1, 1u, l1_entries, page);
                if (ret)
                    goto fail;
                memcpy(leaf_ptrs,
                       page + sizeof(struct infilfs_allocation_page_disk),
                       (size_t)l1_entries * sizeof(__le64));

                for (u32 k = 0; k < l1_entries; ++k, ++global_leaf) {
                    u64 leaf_block =
                        le64_to_cpu(leaf_ptrs[k]);
                    u64 first = (u64)global_leaf *
                        INFILFS_ALLOCATION_BITS_PER_LEAF;
                    u64 remaining = total - first;
                    u32 valid = min_t(u64,
                        INFILFS_ALLOCATION_BITS_PER_LEAF, remaining);

                    layout->leaf_blocks[global_leaf] = leaf_block;
                    ret = infilfs_allocation_read_page(
                        sb, leaf_block, (const u8 *)"INFSAL01",
                        generation, global_leaf, 0u, valid, page);
                    if (ret)
                        goto fail;
                    if (bitmap) {
                        const struct infilfs_allocation_page_disk *leaf =
                            (const struct infilfs_allocation_page_disk *)page;
                        u32 bytes = le32_to_cpu(leaf->bytes_used);
                        size_t offset = global_leaf *
                            (size_t)INFILFS_ALLOCATION_PAGE_DATA_SIZE;

                        if (offset > bitmap_bytes ||
                            bytes > bitmap_bytes - offset) {
                            ret = -EFSCORRUPTED;
                            goto fail;
                        }
                        memcpy(bitmap + offset, leaf + 1, bytes);
                    }
                }
            }
        }
    }

    if (branch_pos != branches || global_l1 != level1 ||
        global_leaf != leaves) {
        ret = -EFSCORRUPTED;
        goto fail;
    }

    all_pages = kvmalloc_array(leaves + branches, sizeof(u64), GFP_KERNEL);
    if (!all_pages) {
        ret = -ENOMEM;
        goto fail;
    }
    memcpy(all_pages, layout->leaf_blocks, leaves * sizeof(u64));
    memcpy(all_pages + leaves, layout->branch_blocks,
           branches * sizeof(u64));
    sort(all_pages, leaves + branches, sizeof(u64),
         infilfs_allocation_u64_compare, NULL);
    for (size_t i = 0; i < leaves + branches; ++i) {
        if (!all_pages[i] || all_pages[i] >= total ||
            (i && all_pages[i] == all_pages[i - 1])) {
            ret = -EFSCORRUPTED;
            goto fail;
        }
        for (u32 c = 0; c < INFILFS_CHECKPOINT_COUNT; ++c) {
            if (all_pages[i] == le64_to_cpu(disk->checkpoint_block[c])) {
                ret = -EFSCORRUPTED;
                goto fail;
            }
        }
    }

    if (bitmap) {
        u64 free_count = 0;
        u64 bit;

        for (bit = total; bit < (u64)bitmap_bytes * 8u; ++bit)
            infilfs_allocation_bitmap_set(bitmap, bit, true);
        for (bit = 0; bit < total; ++bit)
            if (!infilfs_allocation_bitmap_get(bitmap, bit))
                ++free_count;
        if (free_count != le64_to_cpu(disk->free_blocks)) {
            ret = -EFSCORRUPTED;
            goto fail;
        }
        for (size_t i = 0; i < leaves; ++i)
            if (!infilfs_allocation_bitmap_get(bitmap,
                                               layout->leaf_blocks[i])) {
                ret = -EFSCORRUPTED;
                goto fail;
            }
        for (size_t i = 0; i < branches; ++i)
            if (!infilfs_allocation_bitmap_get(bitmap,
                                               layout->branch_blocks[i])) {
                ret = -EFSCORRUPTED;
                goto fail;
            }
        for (u32 i = 0; i < INFILFS_CHECKPOINT_COUNT; ++i)
            if (!infilfs_allocation_bitmap_get(
                    bitmap, le64_to_cpu(disk->checkpoint_block[i]))) {
                ret = -EFSCORRUPTED;
                goto fail;
            }
        if (!infilfs_allocation_bitmap_get(
                bitmap, le64_to_cpu(disk->object_index_block)) ||
            !infilfs_allocation_bitmap_get(
                bitmap, le64_to_cpu(disk->root_object_block))) {
            ret = -EFSCORRUPTED;
            goto fail;
        }
    }

    kvfree(all_pages);
    kfree(page);
    if (bitmap_out) {
        *bitmap_out = bitmap;
        if (bitmap_bytes_out)
            *bitmap_bytes_out = bitmap_bytes;
    } else {
        kvfree(bitmap);
    }
    if (!layout_out)
        infilfs_allocation_layout_destroy(&local);
    return 0;

fail:
    kvfree(all_pages);
    kvfree(bitmap);
    kfree(page);
    infilfs_allocation_layout_destroy(layout);
    return ret;
}
