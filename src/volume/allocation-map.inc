// SPDX-License-Identifier: GPL-3.0-or-later

struct infs_allocation_layout {
    uint64_t *leaf_blocks;
    uint64_t *branch_blocks;
    size_t leaf_count;
    size_t branch_count;
    size_t level1_count;
    size_t level2_count;
};

static void allocation_map_layout_destroy(struct infs_allocation_layout *layout)
{
    if (!layout)
        return;
    free(layout->leaf_blocks);
    free(layout->branch_blocks);
    memset(layout, 0, sizeof(*layout));
}

static infs_status allocation_map_counts(uint64_t total_blocks,
                                         size_t *leaf_count_out,
                                         size_t *level1_count_out,
                                         size_t *level2_count_out,
                                         size_t *branch_count_out)
{
    if (!total_blocks || total_blocks > INFS_ALLOCATION_TREE_MAX_BLOCKS)
        return INFS_STATUS_NOT_SUPPORTED;

    uint64_t leaves = total_blocks / INFS_ALLOCATION_BITS_PER_LEAF +
        ((total_blocks % INFS_ALLOCATION_BITS_PER_LEAF) != 0);
    uint64_t level1 = leaves / INFS_ALLOCATION_TREE_FANOUT +
        ((leaves % INFS_ALLOCATION_TREE_FANOUT) != 0);
    uint64_t level2 = level1 / INFS_ALLOCATION_TREE_FANOUT +
        ((level1 % INFS_ALLOCATION_TREE_FANOUT) != 0);
    uint64_t branches = UINT64_C(1) + level1 + level2;

    if (!leaves || !level1 || !level2 ||
        leaves > SIZE_MAX || level1 > SIZE_MAX || level2 > SIZE_MAX ||
        branches > SIZE_MAX)
        return INFS_STATUS_OVERFLOW;
    if (leaf_count_out)
        *leaf_count_out = (size_t)leaves;
    if (level1_count_out)
        *level1_count_out = (size_t)level1;
    if (level2_count_out)
        *level2_count_out = (size_t)level2;
    if (branch_count_out)
        *branch_count_out = (size_t)branches;
    return INFS_STATUS_OK;
}

static infs_status allocation_map_runtime_bytes(uint64_t total_blocks,
                                                size_t *bytes_out)
{
    if (!bytes_out || total_blocks > UINT64_MAX - 7u)
        return INFS_STATUS_INVALID_ARGUMENT;
    uint64_t raw = (total_blocks + 7u) / 8u;
    uint64_t rounded = raw;
    uint64_t rem = rounded % INFS_BLOCK_SIZE;
    if (rem) {
        uint64_t add = INFS_BLOCK_SIZE - rem;
        if (rounded > UINT64_MAX - add)
            return INFS_STATUS_OVERFLOW;
        rounded += add;
    }
    if (!rounded || rounded > SIZE_MAX)
        return INFS_STATUS_OVERFLOW;
    *bytes_out = (size_t)rounded;
    return INFS_STATUS_OK;
}

static int allocation_map_u64_compare(const void *a, const void *b)
{
    uint64_t av = *(const uint64_t *)a;
    uint64_t bv = *(const uint64_t *)b;
    return (av > bv) - (av < bv);
}

static infs_status allocation_map_read_page(
    struct infs_volume *vol, uint64_t block_no,
    const uint8_t magic[8], uint64_t generation,
    uint64_t logical_index, uint32_t level,
    uint32_t expected_entries, uint8_t block[INFS_BLOCK_SIZE])
{
    if (!vol || !block || block_no == 0 ||
        block_no >= infs_le64_to_cpu(vol->sb.total_blocks))
        return INFS_STATUS_CORRUPT;
    infs_status status = read_block(vol, block_no, block);
    if (status != INFS_STATUS_OK)
        return status;
    if (!infs_validate_allocation_page(block, magic, generation,
                                       logical_index, level))
        return INFS_STATUS_CORRUPT;
    const struct infs_allocation_page_disk *page =
        (const struct infs_allocation_page_disk *)block;
    if (infs_le32_to_cpu(page->entry_count) != expected_entries)
        return INFS_STATUS_CORRUPT;
    return INFS_STATUS_OK;
}

static infs_status allocation_map_load(
    struct infs_volume *vol, const struct infs_superblock_disk *sb,
    uint8_t **bitmap_out, size_t *bitmap_bytes_out,
    struct infs_allocation_layout *layout_out)
{
    if (!vol || !sb)
        return INFS_STATUS_INVALID_ARGUMENT;

    uint64_t total = infs_le64_to_cpu(sb->total_blocks);
    uint64_t generation = infs_le64_to_cpu(sb->generation);
    uint64_t root = infs_le64_to_cpu(sb->allocation_root_block);
    size_t leaves = 0, level1 = 0, level2 = 0, branches = 0;
    infs_status status = allocation_map_counts(
        total, &leaves, &level1, &level2, &branches);
    if (status != INFS_STATUS_OK)
        return status;
    if ((infs_le64_to_cpu(sb->incompat_flags) &
         INFS_INCOMPAT_ALLOCATION_TREE) == 0 ||
        infs_le64_to_cpu(sb->allocation_leaf_count) != leaves ||
        root == 0 || root >= total || generation == 0)
        return INFS_STATUS_CORRUPT;

    struct infs_allocation_layout local = {0};
    struct infs_allocation_layout *layout = layout_out ? layout_out : &local;
    allocation_map_layout_destroy(layout);
    if (leaves > SIZE_MAX / sizeof(uint64_t) ||
        branches > SIZE_MAX / sizeof(uint64_t))
        return INFS_STATUS_OVERFLOW;
    layout->leaf_blocks = calloc(leaves, sizeof(uint64_t));
    layout->branch_blocks = calloc(branches, sizeof(uint64_t));
    if (!layout->leaf_blocks || !layout->branch_blocks) {
        allocation_map_layout_destroy(layout);
        return INFS_STATUS_NO_MEMORY;
    }
    layout->leaf_count = leaves;
    layout->branch_count = branches;
    layout->level1_count = level1;
    layout->level2_count = level2;
    layout->branch_blocks[0] = root;

    uint8_t *bitmap = NULL;
    size_t bitmap_bytes = 0;
    if (bitmap_out) {
        status = allocation_map_runtime_bytes(total, &bitmap_bytes);
        if (status != INFS_STATUS_OK)
            goto fail;
        bitmap = calloc(1u, bitmap_bytes);
        if (!bitmap) {
            status = INFS_STATUS_NO_MEMORY;
            goto fail;
        }
    }

    uint8_t page[INFS_BLOCK_SIZE];
    status = allocation_map_read_page(
        vol, root, INFS_ALLOCATION_BRANCH_PAGE_MAGIC, generation,
        0, INFS_ALLOCATION_TREE_ROOT_LEVEL, (uint32_t)level2, page);
    if (status != INFS_STATUS_OK)
        goto fail;
    const uint64_t *root_children =
        (const uint64_t *)(page + sizeof(struct infs_allocation_page_disk));

    size_t global_level1 = 0;
    size_t global_leaf = 0;
    for (size_t i = 0; i < level2; ++i) {
        uint64_t l2_block = infs_le64_to_cpu(root_children[i]);
        uint32_t l2_entries = (uint32_t)(
            level1 - global_level1 > INFS_ALLOCATION_TREE_FANOUT ?
            INFS_ALLOCATION_TREE_FANOUT : level1 - global_level1);
        size_t l2_slot = 1u + i;
        if (l2_slot >= branches) {
            status = INFS_STATUS_CORRUPT;
            goto fail;
        }
        layout->branch_blocks[l2_slot] = l2_block;
        status = allocation_map_read_page(
            vol, l2_block, INFS_ALLOCATION_BRANCH_PAGE_MAGIC, generation,
            i, 2u, l2_entries, page);
        if (status != INFS_STATUS_OK)
            goto fail;

        uint64_t l1_children[INFS_ALLOCATION_TREE_FANOUT];
        const uint64_t *disk_l1 =
            (const uint64_t *)(page + sizeof(struct infs_allocation_page_disk));
        memcpy(l1_children, disk_l1, (size_t)l2_entries * sizeof(uint64_t));

        for (uint32_t j = 0; j < l2_entries; ++j, ++global_level1) {
            uint64_t l1_block = infs_le64_to_cpu(l1_children[j]);
            uint32_t l1_entries = (uint32_t)(
                leaves - global_leaf > INFS_ALLOCATION_TREE_FANOUT ?
                INFS_ALLOCATION_TREE_FANOUT : leaves - global_leaf);
            size_t l1_slot = 1u + level2 + global_level1;
            if (l1_slot >= branches) {
                status = INFS_STATUS_CORRUPT;
                goto fail;
            }
            layout->branch_blocks[l1_slot] = l1_block;
            status = allocation_map_read_page(
                vol, l1_block, INFS_ALLOCATION_BRANCH_PAGE_MAGIC, generation,
                global_level1, 1u, l1_entries, page);
            if (status != INFS_STATUS_OK)
                goto fail;

            uint64_t leaf_children[INFS_ALLOCATION_TREE_FANOUT];
            const uint64_t *disk_leaf =
                (const uint64_t *)(page + sizeof(struct infs_allocation_page_disk));
            memcpy(leaf_children, disk_leaf,
                   (size_t)l1_entries * sizeof(uint64_t));

            for (uint32_t k = 0; k < l1_entries; ++k, ++global_leaf) {
                uint64_t leaf_block = infs_le64_to_cpu(leaf_children[k]);
                uint64_t first_bit =
                    (uint64_t)global_leaf * INFS_ALLOCATION_BITS_PER_LEAF;
                uint64_t remaining = total - first_bit;
                uint32_t valid_bits = (uint32_t)(
                    remaining > INFS_ALLOCATION_BITS_PER_LEAF ?
                    INFS_ALLOCATION_BITS_PER_LEAF : remaining);
                layout->leaf_blocks[global_leaf] = leaf_block;
                status = allocation_map_read_page(
                    vol, leaf_block, INFS_ALLOCATION_LEAF_PAGE_MAGIC,
                    generation, global_leaf, 0u, valid_bits, page);
                if (status != INFS_STATUS_OK)
                    goto fail;

                if (bitmap) {
                    const struct infs_allocation_page_disk *leaf =
                        (const struct infs_allocation_page_disk *)page;
                    uint32_t bytes = infs_le32_to_cpu(leaf->bytes_used);
                    size_t offset =
                        global_leaf * (size_t)INFS_ALLOCATION_PAGE_DATA_SIZE;
                    if (offset > bitmap_bytes ||
                        bytes > bitmap_bytes - offset) {
                        status = INFS_STATUS_CORRUPT;
                        goto fail;
                    }
                    memcpy(bitmap + offset, leaf + 1, bytes);
                }
            }
        }
    }
    if (global_level1 != level1 || global_leaf != leaves) {
        status = INFS_STATUS_CORRUPT;
        goto fail;
    }

    size_t page_count = leaves + branches;
    if (page_count > SIZE_MAX / sizeof(uint64_t)) {
        status = INFS_STATUS_OVERFLOW;
        goto fail;
    }
    uint64_t *all_pages = malloc(page_count * sizeof(uint64_t));
    if (!all_pages) {
        status = INFS_STATUS_NO_MEMORY;
        goto fail;
    }
    memcpy(all_pages, layout->leaf_blocks, leaves * sizeof(uint64_t));
    memcpy(all_pages + leaves, layout->branch_blocks,
           branches * sizeof(uint64_t));
    qsort(all_pages, page_count, sizeof(uint64_t), allocation_map_u64_compare);
    for (size_t i = 0; i < page_count; ++i) {
        if (all_pages[i] == 0 || all_pages[i] >= total ||
            (i && all_pages[i] == all_pages[i - 1u])) {
            free(all_pages);
            status = INFS_STATUS_CORRUPT;
            goto fail;
        }
        for (unsigned c = 0; c < INFS_CHECKPOINT_COUNT; ++c) {
            if (all_pages[i] == infs_le64_to_cpu(sb->checkpoint_block[c])) {
                free(all_pages);
                status = INFS_STATUS_CORRUPT;
                goto fail;
            }
        }
    }
    free(all_pages);

    if (bitmap) {
        for (uint64_t bit = total;
             bit < (uint64_t)bitmap_bytes * UINT64_C(8); ++bit)
            bitmap_set(bitmap, bit, 1);

        uint64_t free_count = 0;
        for (uint64_t bit = 0; bit < total; ++bit)
            if (!bitmap_get(bitmap, bit))
                ++free_count;
        if (free_count != infs_le64_to_cpu(sb->free_blocks)) {
            status = INFS_STATUS_CORRUPT;
            goto fail;
        }

        for (size_t i = 0; i < leaves; ++i)
            if (!bitmap_get(bitmap, layout->leaf_blocks[i])) {
                status = INFS_STATUS_CORRUPT;
                goto fail;
            }
        for (size_t i = 0; i < branches; ++i)
            if (!bitmap_get(bitmap, layout->branch_blocks[i])) {
                status = INFS_STATUS_CORRUPT;
                goto fail;
            }

        uint64_t critical[] = {
            infs_le64_to_cpu(sb->checkpoint_block[0]),
            infs_le64_to_cpu(sb->checkpoint_block[1]),
            infs_le64_to_cpu(sb->checkpoint_block[2]),
            infs_le64_to_cpu(sb->object_index_block),
            infs_le64_to_cpu(sb->root_object_block)
        };
        for (size_t i = 0; i < sizeof(critical) / sizeof(critical[0]); ++i) {
            if (critical[i] >= total || !bitmap_get(bitmap, critical[i])) {
                status = INFS_STATUS_CORRUPT;
                goto fail;
            }
        }
    }

    if (bitmap_out) {
        *bitmap_out = bitmap;
        if (bitmap_bytes_out)
            *bitmap_bytes_out = bitmap_bytes;
    } else {
        free(bitmap);
    }
    if (!layout_out)
        allocation_map_layout_destroy(&local);
    return INFS_STATUS_OK;

fail:
    free(bitmap);
    allocation_map_layout_destroy(layout);
    return status;
}

static size_t allocation_map_leaf_index(uint64_t block)
{
    return (size_t)(block / INFS_ALLOCATION_BITS_PER_LEAF);
}

static void allocation_map_mark_dirty_range(
    uint8_t *dirty, size_t leaf_count, uint64_t total,
    uint64_t start, uint64_t count)
{
    if (!dirty || !count || start >= total)
        return;
    uint64_t end = start + count;
    if (end < start || end > total)
        end = total;
    size_t first = allocation_map_leaf_index(start);
    size_t last = allocation_map_leaf_index(end - 1u);
    if (first >= leaf_count)
        return;
    if (last >= leaf_count)
        last = leaf_count - 1u;
    for (size_t i = first; i <= last; ++i)
        dirty[i] = 1;
}

static void allocation_map_mark_tx_changes(
    const struct infs_volume *vol, uint8_t *dirty, size_t leaf_count)
{
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    for (size_t i = 0; i < vol->tx_allocated_count; ++i)
        allocation_map_mark_dirty_range(
            dirty, leaf_count, total,
            vol->tx_allocated[i].start, vol->tx_allocated[i].count);
    for (size_t i = 0; i < vol->tx_deferred_count; ++i)
        allocation_map_mark_dirty_range(
            dirty, leaf_count, total,
            vol->tx_deferred[i].start, vol->tx_deferred[i].count);
}

static void allocation_map_close_dirty_set(
    const struct infs_allocation_layout *old,
    uint8_t *dirty_leaves, uint8_t *dirty_level1,
    uint8_t *dirty_level2, uint64_t total)
{
    if (!old || !dirty_leaves || !dirty_level1 || !dirty_level2)
        return;

    int changed;
    const size_t level1_base = 1u + old->level2_count;
    do {
        changed = 0;

        /* A changed leaf requires its complete pointer path to the root. */
        for (size_t i = 0; i < old->leaf_count; ++i) {
            if (!dirty_leaves[i])
                continue;
            size_t l1 = i / INFS_ALLOCATION_TREE_FANOUT;
            if (l1 < old->level1_count && !dirty_level1[l1]) {
                dirty_level1[l1] = 1;
                changed = 1;
            }
        }
        for (size_t i = 0; i < old->level1_count; ++i) {
            if (!dirty_level1[i])
                continue;
            size_t l2 = i / INFS_ALLOCATION_TREE_FANOUT;
            if (l2 < old->level2_count && !dirty_level2[l2]) {
                dirty_level2[l2] = 1;
                changed = 1;
            }
        }

        /*
         * Replacing an allocation-map page changes the allocation bit for its
         * old physical block. Close that self-hosting dependency over leaves.
         * The root is always replaced because the checkpoint publishes a new
         * allocation root for the transaction.
         */
        uint64_t block = old->branch_blocks[0];
        if (block < total) {
            size_t owner = allocation_map_leaf_index(block);
            if (owner < old->leaf_count && !dirty_leaves[owner]) {
                dirty_leaves[owner] = 1;
                changed = 1;
            }
        }
        for (size_t i = 0; i < old->level2_count; ++i) {
            if (!dirty_level2[i])
                continue;
            block = old->branch_blocks[1u + i];
            if (block < total) {
                size_t owner = allocation_map_leaf_index(block);
                if (owner < old->leaf_count && !dirty_leaves[owner]) {
                    dirty_leaves[owner] = 1;
                    changed = 1;
                }
            }
        }
        for (size_t i = 0; i < old->level1_count; ++i) {
            if (!dirty_level1[i])
                continue;
            block = old->branch_blocks[level1_base + i];
            if (block < total) {
                size_t owner = allocation_map_leaf_index(block);
                if (owner < old->leaf_count && !dirty_leaves[owner]) {
                    dirty_leaves[owner] = 1;
                    changed = 1;
                }
            }
        }

        /* A replaced leaf also frees its old physical page. */
        for (size_t i = 0; i < old->leaf_count; ++i) {
            if (!dirty_leaves[i])
                continue;
            block = old->leaf_blocks[i];
            if (block >= total)
                continue;
            size_t owner = allocation_map_leaf_index(block);
            if (owner < old->leaf_count && !dirty_leaves[owner]) {
                dirty_leaves[owner] = 1;
                changed = 1;
            }
        }
    } while (changed);
}

static size_t allocation_map_dirty_count(const uint8_t *dirty, size_t count)
{
    size_t result = 0;
    for (size_t i = 0; i < count; ++i)
        result += dirty[i] != 0;
    return result;
}

static infs_status allocation_map_write_page(
    struct infs_volume *vol, uint64_t physical,
    const uint8_t magic[8], uint64_t generation,
    uint64_t logical, uint32_t level, uint32_t entries,
    const void *payload, uint32_t bytes)
{
    uint8_t block[INFS_BLOCK_SIZE];
    infs_status status = infs_allocation_page_init(
        block, magic, generation, logical, level);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_allocation_page_disk *page =
        (struct infs_allocation_page_disk *)block;
    page->entry_count = infs_cpu_to_le32(entries);
    page->bytes_used = infs_cpu_to_le32(bytes);
    if (bytes)
        memcpy(page + 1, payload, bytes);
    status = infs_allocation_page_finalize(block);
    if (status != INFS_STATUS_OK)
        return status;
    return write_block(vol, physical, block);
}

static infs_status allocation_map_publish(
    struct infs_volume *vol, uint64_t generation)
{
    if (!vol || !vol->tx_active || generation == 0)
        return INFS_STATUS_INVALID_ARGUMENT;

    struct infs_allocation_layout old = {0};
    infs_status status = allocation_map_load(
        vol, &vol->tx_base_sb, NULL, NULL, &old);
    if (status != INFS_STATUS_OK)
        return status;

    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    uint8_t *dirty_leaves = calloc(old.leaf_count, 1u);
    uint8_t *dirty_level1 = calloc(old.level1_count, 1u);
    uint8_t *dirty_level2 = calloc(old.level2_count, 1u);
    uint64_t *reserved = NULL;
    size_t reserved_count = 0;
    size_t reserved_capacity = 0;
    uint64_t *new_leaves = NULL;
    uint64_t *new_branches = NULL;
    uint8_t *snapshot_union = NULL;
    if (!dirty_leaves || !dirty_level1 || !dirty_level2) {
        status = INFS_STATUS_NO_MEMORY;
        goto out;
    }

    allocation_map_mark_tx_changes(vol, dirty_leaves, old.leaf_count);
    allocation_map_close_dirty_set(
        &old, dirty_leaves, dirty_level1, dirty_level2, total);

    size_t needed = 0;
    for (;;) {
        size_t dirty_leaf_count =
            allocation_map_dirty_count(dirty_leaves, old.leaf_count);
        size_t dirty_l1_count =
            allocation_map_dirty_count(dirty_level1, old.level1_count);
        size_t dirty_l2_count =
            allocation_map_dirty_count(dirty_level2, old.level2_count);
        if (dirty_leaf_count > SIZE_MAX - dirty_l1_count ||
            dirty_leaf_count + dirty_l1_count > SIZE_MAX - dirty_l2_count ||
            dirty_leaf_count + dirty_l1_count + dirty_l2_count >
                SIZE_MAX - 1u) {
            status = INFS_STATUS_OVERFLOW;
            goto out;
        }
        needed = dirty_leaf_count + dirty_l1_count + dirty_l2_count + 1u;
        if (needed > old.leaf_count + old.branch_count) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }

        while (reserved_count < needed) {
            if (reserved_count == reserved_capacity) {
                size_t next = reserved_capacity ? reserved_capacity * 2u : 32u;
                if (next < reserved_capacity ||
                    next > SIZE_MAX / sizeof(*reserved)) {
                    status = INFS_STATUS_OVERFLOW;
                    goto out;
                }
                uint64_t *grown = realloc(
                    reserved, next * sizeof(*reserved));
                if (!grown) {
                    status = INFS_STATUS_NO_MEMORY;
                    goto out;
                }
                reserved = grown;
                reserved_capacity = next;
            }
            uint64_t block = 0;
            status = alloc_exact_run_uninitialized(vol, 1u, &block);
            if (status != INFS_STATUS_OK)
                goto out;
            reserved[reserved_count++] = block;
        }

        size_t before = needed;
        allocation_map_mark_tx_changes(vol, dirty_leaves, old.leaf_count);
        allocation_map_close_dirty_set(
            &old, dirty_leaves, dirty_level1, dirty_level2, total);

        dirty_leaf_count =
            allocation_map_dirty_count(dirty_leaves, old.leaf_count);
        dirty_l1_count =
            allocation_map_dirty_count(dirty_level1, old.level1_count);
        dirty_l2_count =
            allocation_map_dirty_count(dirty_level2, old.level2_count);
        if (dirty_leaf_count > SIZE_MAX - dirty_l1_count ||
            dirty_leaf_count + dirty_l1_count > SIZE_MAX - dirty_l2_count ||
            dirty_leaf_count + dirty_l1_count + dirty_l2_count >
                SIZE_MAX - 1u) {
            status = INFS_STATUS_OVERFLOW;
            goto out;
        }
        size_t after =
            dirty_leaf_count + dirty_l1_count + dirty_l2_count + 1u;
        if (after == before && reserved_count >= after) {
            needed = after;
            break;
        }
    }

    new_leaves = malloc(old.leaf_count * sizeof(*new_leaves));
    new_branches = malloc(old.branch_count * sizeof(*new_branches));
    if (!new_leaves || !new_branches) {
        status = INFS_STATUS_NO_MEMORY;
        goto out;
    }
    memcpy(new_leaves, old.leaf_blocks,
           old.leaf_count * sizeof(*new_leaves));
    memcpy(new_branches, old.branch_blocks,
           old.branch_count * sizeof(*new_branches));

    size_t r = 0;
    for (size_t i = 0; i < old.leaf_count; ++i)
        if (dirty_leaves[i])
            new_leaves[i] = reserved[r++];

    new_branches[0] = reserved[r++];
    for (size_t i = 0; i < old.level2_count; ++i)
        if (dirty_level2[i])
            new_branches[1u + i] = reserved[r++];

    const size_t level1_base = 1u + old.level2_count;
    for (size_t i = 0; i < old.level1_count; ++i)
        if (dirty_level1[i])
            new_branches[level1_base + i] = reserved[r++];

    if (r != needed || r > reserved_count) {
        status = INFS_STATUS_CORRUPT;
        goto out;
    }

    /*
     * Defer only pages actually replaced. Unchanged branches and leaves remain
     * valid shared metadata and may carry an older generation number.
     */
    status = tx_defer_range(vol, old.branch_blocks[0], 1u);
    if (status != INFS_STATUS_OK)
        goto out;
    for (size_t i = 0; i < old.level2_count; ++i) {
        if (!dirty_level2[i])
            continue;
        status = tx_defer_range(vol, old.branch_blocks[1u + i], 1u);
        if (status != INFS_STATUS_OK)
            goto out;
    }
    for (size_t i = 0; i < old.level1_count; ++i) {
        if (!dirty_level1[i])
            continue;
        status = tx_defer_range(
            vol, old.branch_blocks[level1_base + i], 1u);
        if (status != INFS_STATUS_OK)
            goto out;
    }
    for (size_t i = 0; i < old.leaf_count; ++i) {
        if (!dirty_leaves[i])
            continue;
        status = tx_defer_range(vol, old.leaf_blocks[i], 1u);
        if (status != INFS_STATUS_OK)
            goto out;
    }

    status = snapshot_union_bitmap(vol, &snapshot_union);
    if (status != INFS_STATUS_OK)
        goto out;

    uint64_t free_blocks = infs_le64_to_cpu(vol->sb.free_blocks);
    for (size_t i = 0; i < vol->tx_deferred_count; ++i) {
        uint64_t start = vol->tx_deferred[i].start;
        uint64_t count = vol->tx_deferred[i].count;
        for (uint64_t b = 0; b < count && start + b < total; ++b) {
            uint64_t block = start + b;
            if (!bitmap_get(vol->bitmap, block) ||
                bitmap_get(snapshot_union, block))
                continue;
            bitmap_set(vol->bitmap, block, 0);
            if (vol->free_extent_index_valid &&
                !free_extent_index_add(vol, block, 1u))
                free_extent_index_invalidate(vol);
            if (free_blocks == UINT64_MAX) {
                status = INFS_STATUS_OVERFLOW;
                goto out;
            }
            ++free_blocks;
        }
    }
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        bitmap_set(vol->bitmap,
                   infs_le64_to_cpu(vol->sb.checkpoint_block[i]), 1);
    for (uint64_t b = total;
         b < (uint64_t)vol->bitmap_bytes * UINT64_C(8); ++b)
        bitmap_set(vol->bitmap, b, 1);

    vol->sb.free_blocks = infs_cpu_to_le64(free_blocks);
    vol->sb.allocation_root_block = infs_cpu_to_le64(new_branches[0]);
    vol->sb.allocation_leaf_count = infs_cpu_to_le64(old.leaf_count);
    vol->sb.generation = infs_cpu_to_le64(generation);

    for (size_t i = 0; i < old.leaf_count; ++i) {
        if (!dirty_leaves[i])
            continue;
        uint64_t first_bit =
            (uint64_t)i * INFS_ALLOCATION_BITS_PER_LEAF;
        uint64_t remaining = total - first_bit;
        uint32_t valid_bits = (uint32_t)(
            remaining > INFS_ALLOCATION_BITS_PER_LEAF ?
            INFS_ALLOCATION_BITS_PER_LEAF : remaining);
        uint32_t bytes = (valid_bits + 7u) / 8u;
        size_t offset = i * (size_t)INFS_ALLOCATION_PAGE_DATA_SIZE;
        if (offset > vol->bitmap_bytes ||
            bytes > vol->bitmap_bytes - offset) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }
        status = allocation_map_write_page(
            vol, new_leaves[i], INFS_ALLOCATION_LEAF_PAGE_MAGIC,
            generation, i, 0u, valid_bits, vol->bitmap + offset, bytes);
        if (status != INFS_STATUS_OK)
            goto out;
    }

    for (size_t i = 0; i < old.level1_count; ++i) {
        if (!dirty_level1[i])
            continue;
        size_t first = i * INFS_ALLOCATION_TREE_FANOUT;
        uint32_t entries = (uint32_t)(
            old.leaf_count - first > INFS_ALLOCATION_TREE_FANOUT ?
            INFS_ALLOCATION_TREE_FANOUT : old.leaf_count - first);
        uint64_t pointers[INFS_ALLOCATION_TREE_FANOUT];
        for (uint32_t j = 0; j < entries; ++j)
            pointers[j] = infs_cpu_to_le64(new_leaves[first + j]);
        status = allocation_map_write_page(
            vol, new_branches[level1_base + i],
            INFS_ALLOCATION_BRANCH_PAGE_MAGIC, generation,
            i, 1u, entries, pointers, entries * sizeof(uint64_t));
        if (status != INFS_STATUS_OK)
            goto out;
    }

    for (size_t i = 0; i < old.level2_count; ++i) {
        if (!dirty_level2[i])
            continue;
        size_t first = i * INFS_ALLOCATION_TREE_FANOUT;
        uint32_t entries = (uint32_t)(
            old.level1_count - first > INFS_ALLOCATION_TREE_FANOUT ?
            INFS_ALLOCATION_TREE_FANOUT : old.level1_count - first);
        uint64_t pointers[INFS_ALLOCATION_TREE_FANOUT];
        for (uint32_t j = 0; j < entries; ++j)
            pointers[j] = infs_cpu_to_le64(
                new_branches[level1_base + first + j]);
        status = allocation_map_write_page(
            vol, new_branches[1u + i],
            INFS_ALLOCATION_BRANCH_PAGE_MAGIC, generation,
            i, 2u, entries, pointers, entries * sizeof(uint64_t));
        if (status != INFS_STATUS_OK)
            goto out;
    }

    {
        uint64_t pointers[INFS_ALLOCATION_TREE_FANOUT];
        for (uint32_t i = 0; i < (uint32_t)old.level2_count; ++i)
            pointers[i] = infs_cpu_to_le64(new_branches[1u + i]);
        status = allocation_map_write_page(
            vol, new_branches[0], INFS_ALLOCATION_BRANCH_PAGE_MAGIC,
            generation, 0u, INFS_ALLOCATION_TREE_ROOT_LEVEL,
            (uint32_t)old.level2_count, pointers,
            (uint32_t)old.level2_count * sizeof(uint64_t));
    }

out:
    free(snapshot_union);
    free(new_branches);
    free(new_leaves);
    free(reserved);
    free(dirty_level2);
    free(dirty_level1);
    free(dirty_leaves);
    allocation_map_layout_destroy(&old);
    return status;
}
