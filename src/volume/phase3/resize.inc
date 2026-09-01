// SPDX-License-Identifier: GPL-3.0-or-later

#define INFS_RESIZE_MIN_BYTES (UINT64_C(16) * 1024u * 1024u)

static int resize_is_old_allocation_page(const struct infs_volume *vol,
                                         uint64_t block)
{
    for (size_t i = 0; i < vol->allocation_leaf_count; ++i)
        if (vol->allocation_leaf_blocks[i] == block)
            return 1;
    for (size_t i = 0; i < vol->allocation_branch_count; ++i)
        if (vol->allocation_branch_blocks[i] == block)
            return 1;
    return 0;
}

static int resize_is_old_checkpoint(const struct infs_volume *vol,
                                    uint64_t block)
{
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        if (infs_le64_to_cpu(vol->sb.checkpoint_block[i]) == block)
            return 1;
    return 0;
}

static uint64_t resize_pick_free_near(uint8_t *bitmap, uint64_t total,
                                      uint64_t preferred)
{
    if (!bitmap || total <= 1u)
        return 0;
    if (preferred >= total)
        preferred = total - 1u;

    for (uint64_t distance = 0; distance < total; ++distance) {
        if (preferred >= distance) {
            uint64_t block = preferred - distance;
            if (block != 0 && !bitmap_get(bitmap, block)) {
                bitmap_set(bitmap, block, 1);
                return block;
            }
        }
        if (distance && preferred <= UINT64_MAX - distance) {
            uint64_t block = preferred + distance;
            if (block < total && block != 0 &&
                !bitmap_get(bitmap, block)) {
                bitmap_set(bitmap, block, 1);
                return block;
            }
        }
    }
    return 0;
}

static uint64_t resize_pick_free_reverse(uint8_t *bitmap, uint64_t total,
                                         uint64_t *cursor)
{
    if (!bitmap || !cursor || total <= 1u)
        return 0;
    uint64_t block = *cursor < total ? *cursor : total - 1u;
    while (block > 0) {
        if (!bitmap_get(bitmap, block)) {
            bitmap_set(bitmap, block, 1);
            *cursor = block - 1u;
            return block;
        }
        --block;
    }
    return 0;
}

static uint64_t resize_count_free(const uint8_t *bitmap, uint64_t total)
{
    uint64_t count = 0;
    for (uint64_t block = 0; block < total; ++block)
        if (!bitmap_get(bitmap, block))
            ++count;
    return count;
}

static int resize_has_named_snapshots(struct infs_volume *vol)
{
    if (!snapshots_enabled(vol))
        return 0;

    uint8_t catalog[INFS_BLOCK_SIZE];
    struct infs_snapshot_catalog_payload_disk *payload = NULL;
    infs_status status = snapshot_catalog_load(
        vol, catalog, &payload, NULL);
    if (status == INFS_STATUS_NOT_FOUND)
        return 0;
    if (status != INFS_STATUS_OK)
        return -1;
    return infs_le32_to_cpu(payload->snapshot_count) != 0;
}

static infs_status resize_allocate_layout(
    uint8_t *bitmap, uint64_t total,
    struct infs_allocation_layout *layout)
{
    size_t leaves = 0, level1 = 0, level2 = 0, branches = 0;
    infs_status status = allocation_map_counts(
        total, &leaves, &level1, &level2, &branches);
    if (status != INFS_STATUS_OK)
        return status;
    if (leaves > SIZE_MAX / sizeof(uint64_t) ||
        branches > SIZE_MAX / sizeof(uint64_t))
        return INFS_STATUS_OVERFLOW;

    memset(layout, 0, sizeof(*layout));
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

    uint64_t cursor = total - 1u;
    for (size_t i = 0; i < leaves; ++i) {
        uint64_t block = resize_pick_free_reverse(bitmap, total, &cursor);
        if (!block) {
            allocation_map_layout_destroy(layout);
            return INFS_STATUS_NO_SPACE;
        }
        layout->leaf_blocks[i] = block;
    }
    for (size_t i = 0; i < branches; ++i) {
        uint64_t block = resize_pick_free_reverse(bitmap, total, &cursor);
        if (!block) {
            allocation_map_layout_destroy(layout);
            return INFS_STATUS_NO_SPACE;
        }
        layout->branch_blocks[i] = block;
    }
    return INFS_STATUS_OK;
}

/*
 * Resize may grow the filesystem beyond the currently committed geometry.
 * The ordinary write_block() helper intentionally rejects such blocks, so the
 * resize transaction needs a bounded raw page writer whose authority is the
 * proposed geometry plus the already-validated backing-store size.
 */
static infs_status resize_write_allocation_page(
    struct infs_volume *vol, uint64_t total, uint64_t physical,
    const uint8_t magic[8], uint64_t generation,
    uint64_t logical, uint32_t level, uint32_t entries,
    const void *payload, uint32_t bytes)
{
    uint8_t block[INFS_BLOCK_SIZE];

    if (!vol || !physical || physical >= total)
        return INFS_STATUS_INVALID_ARGUMENT;
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
    return infs_storage_write(
        &vol->storage, physical * INFS_BLOCK_SIZE,
        block, sizeof(block));
}

static infs_status resize_write_layout(
    struct infs_volume *vol, const uint8_t *bitmap, size_t bitmap_bytes,
    uint64_t total, uint64_t generation,
    const struct infs_allocation_layout *layout)
{
    if (!vol || !bitmap || !layout || !layout->branch_count)
        return INFS_STATUS_INVALID_ARGUMENT;

    for (size_t i = 0; i < layout->leaf_count; ++i) {
        uint64_t first_bit =
            (uint64_t)i * INFS_ALLOCATION_BITS_PER_LEAF;
        uint64_t remaining = total - first_bit;
        uint32_t valid_bits = (uint32_t)(
            remaining > INFS_ALLOCATION_BITS_PER_LEAF ?
            INFS_ALLOCATION_BITS_PER_LEAF : remaining);
        uint32_t bytes = (valid_bits + 7u) / 8u;
        size_t offset = i * (size_t)INFS_ALLOCATION_PAGE_DATA_SIZE;
        if (offset > bitmap_bytes || bytes > bitmap_bytes - offset)
            return INFS_STATUS_CORRUPT;
        infs_status status = resize_write_allocation_page(
            vol, total, layout->leaf_blocks[i],
            INFS_ALLOCATION_LEAF_PAGE_MAGIC, generation,
            i, 0u, valid_bits, bitmap + offset, bytes);
        if (status != INFS_STATUS_OK)
            return status;
    }

    const size_t level1_base = 1u + layout->level2_count;
    for (size_t i = 0; i < layout->level1_count; ++i) {
        size_t first = i * INFS_ALLOCATION_TREE_FANOUT;
        uint32_t entries = (uint32_t)(
            layout->leaf_count - first > INFS_ALLOCATION_TREE_FANOUT ?
            INFS_ALLOCATION_TREE_FANOUT : layout->leaf_count - first);
        uint64_t pointers[INFS_ALLOCATION_TREE_FANOUT];
        for (uint32_t j = 0; j < entries; ++j)
            pointers[j] =
                infs_cpu_to_le64(layout->leaf_blocks[first + j]);
        infs_status status = resize_write_allocation_page(
            vol, total, layout->branch_blocks[level1_base + i],
            INFS_ALLOCATION_BRANCH_PAGE_MAGIC, generation,
            i, 1u, entries, pointers,
            entries * (uint32_t)sizeof(uint64_t));
        if (status != INFS_STATUS_OK)
            return status;
    }

    for (size_t i = 0; i < layout->level2_count; ++i) {
        size_t first = i * INFS_ALLOCATION_TREE_FANOUT;
        uint32_t entries = (uint32_t)(
            layout->level1_count - first > INFS_ALLOCATION_TREE_FANOUT ?
            INFS_ALLOCATION_TREE_FANOUT : layout->level1_count - first);
        uint64_t pointers[INFS_ALLOCATION_TREE_FANOUT];
        for (uint32_t j = 0; j < entries; ++j)
            pointers[j] = infs_cpu_to_le64(
                layout->branch_blocks[level1_base + first + j]);
        infs_status status = resize_write_allocation_page(
            vol, total, layout->branch_blocks[1u + i],
            INFS_ALLOCATION_BRANCH_PAGE_MAGIC, generation,
            i, 2u, entries, pointers,
            entries * (uint32_t)sizeof(uint64_t));
        if (status != INFS_STATUS_OK)
            return status;
    }

    {
        uint64_t pointers[INFS_ALLOCATION_TREE_FANOUT];
        for (uint32_t i = 0;
             i < (uint32_t)layout->level2_count; ++i)
            pointers[i] =
                infs_cpu_to_le64(layout->branch_blocks[1u + i]);
        return resize_write_allocation_page(
            vol, total, layout->branch_blocks[0],
            INFS_ALLOCATION_BRANCH_PAGE_MAGIC, generation,
            0u, INFS_ALLOCATION_TREE_ROOT_LEVEL,
            (uint32_t)layout->level2_count, pointers,
            (uint32_t)layout->level2_count *
                (uint32_t)sizeof(uint64_t));
    }
}

infs_status infs_volume_resize(struct infs_volume *vol,
                               uint64_t new_size_bytes)
{
    struct infs_allocation_layout next_layout = {0};
    uint8_t *next_bitmap = NULL;
    size_t next_bitmap_bytes = 0;
    uint64_t storage_bytes = 0;
    int is_device = 0;
    infs_status status;

    if (!vol || !infs_storage_valid(&vol->storage) ||
        !vol->writable || (new_size_bytes % INFS_BLOCK_SIZE) != 0 ||
        new_size_bytes < INFS_RESIZE_MIN_BYTES)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (vol->snapshot_view)
        return INFS_STATUS_READ_ONLY;

    status = infs_volume_sync(vol);
    if (status != INFS_STATUS_OK)
        return status;
    status = infs_storage_get_size(
        &vol->storage, &storage_bytes, &is_device);
    if (status != INFS_STATUS_OK)
        return status;
    (void)is_device;
    if (new_size_bytes > storage_bytes)
        return INFS_STATUS_NO_SPACE;
    if ((storage_bytes % INFS_BLOCK_SIZE) != 0)
        return INFS_STATUS_INVALID_ARGUMENT;

    uint64_t old_total = infs_le64_to_cpu(vol->sb.total_blocks);
    uint64_t new_total = new_size_bytes / INFS_BLOCK_SIZE;
    if (new_total == old_total)
        return INFS_STATUS_OK;
    if (new_total > INFS_ALLOCATION_TREE_MAX_BLOCKS)
        return INFS_STATUS_NOT_SUPPORTED;

    /*
     * Snapshot records in Format 0.17 carry an immutable allocation image but
     * not their historical total-block geometry. Resizing that geometry
     * without rewriting the retained image would make snapshot validation
     * ambiguous. Fail closed until the snapshot image is explicitly rebased.
     */
    int snapshot_state = resize_has_named_snapshots(vol);
    if (snapshot_state < 0)
        return INFS_STATUS_CORRUPT;
    if (snapshot_state)
        return INFS_STATUS_BUSY;

    if (new_total < old_total) {
        for (uint64_t block = new_total; block < old_total; ++block) {
            if (!bitmap_get(vol->bitmap, block))
                continue;
            if (resize_is_old_allocation_page(vol, block) ||
                resize_is_old_checkpoint(vol, block))
                continue;
            return INFS_STATUS_BUSY;
        }
    }

    status = allocation_map_runtime_bytes(
        new_total, &next_bitmap_bytes);
    if (status != INFS_STATUS_OK)
        return status;
    next_bitmap = calloc(1u, next_bitmap_bytes);
    if (!next_bitmap)
        return INFS_STATUS_NO_MEMORY;

    uint64_t common =
        old_total < new_total ? old_total : new_total;
    for (uint64_t block = 0; block < common; ++block)
        if (bitmap_get(vol->bitmap, block))
            bitmap_set(next_bitmap, block, 1);
    for (uint64_t block = new_total;
         block < (uint64_t)next_bitmap_bytes * UINT64_C(8); ++block)
        bitmap_set(next_bitmap, block, 1);

    uint64_t checkpoint[INFS_CHECKPOINT_COUNT] = {
        0,
        infs_le64_to_cpu(vol->sb.checkpoint_block[1]),
        infs_le64_to_cpu(vol->sb.checkpoint_block[2])
    };
    bitmap_set(next_bitmap, 0, 1);

    if (checkpoint[1] >= new_total) {
        checkpoint[1] = resize_pick_free_near(
            next_bitmap, new_total, new_total / 2u);
        if (!checkpoint[1]) {
            status = INFS_STATUS_NO_SPACE;
            goto out;
        }
    } else {
        bitmap_set(next_bitmap, checkpoint[1], 1);
    }
    if (checkpoint[2] >= new_total ||
        checkpoint[2] == checkpoint[1]) {
        checkpoint[2] = resize_pick_free_near(
            next_bitmap, new_total, new_total - 1u);
        if (!checkpoint[2]) {
            status = INFS_STATUS_NO_SPACE;
            goto out;
        }
    } else {
        bitmap_set(next_bitmap, checkpoint[2], 1);
    }

    /*
     * Keep the current allocation tree reserved until every replacement page
     * has a distinct previously-free home. A crash before block-zero
     * publication can therefore always return to the old checkpoint graph.
     */
    status = resize_allocate_layout(
        next_bitmap, new_total, &next_layout);
    if (status != INFS_STATUS_OK)
        goto out;

    for (size_t i = 0; i < vol->allocation_leaf_count; ++i) {
        uint64_t block = vol->allocation_leaf_blocks[i];
        if (block < new_total)
            bitmap_set(next_bitmap, block, 0);
    }
    for (size_t i = 0; i < vol->allocation_branch_count; ++i) {
        uint64_t block = vol->allocation_branch_blocks[i];
        if (block < new_total)
            bitmap_set(next_bitmap, block, 0);
    }
    for (unsigned i = 1; i < INFS_CHECKPOINT_COUNT; ++i) {
        uint64_t old = infs_le64_to_cpu(vol->sb.checkpoint_block[i]);
        if (old < new_total &&
            old != checkpoint[1] && old != checkpoint[2])
            bitmap_set(next_bitmap, old, 0);
    }

    uint64_t generation;
    if (!infiltratr_u64_add_checked(
            infs_le64_to_cpu(vol->sb.generation),
            UINT64_C(1), &generation) || generation == 0) {
        status = INFS_STATUS_OVERFLOW;
        goto out;
    }

    status = resize_write_layout(
        vol, next_bitmap, next_bitmap_bytes,
        new_total, generation, &next_layout);
    if (status != INFS_STATUS_OK)
        goto out;
    status = infs_storage_flush(&vol->storage);
    if (status != INFS_STATUS_OK)
        goto out;

    struct infs_superblock_disk next_sb = vol->sb;
    next_sb.total_blocks = infs_cpu_to_le64(new_total);
    next_sb.free_blocks =
        infs_cpu_to_le64(resize_count_free(next_bitmap, new_total));
    next_sb.allocation_root_block =
        infs_cpu_to_le64(next_layout.branch_blocks[0]);
    next_sb.allocation_leaf_count =
        infs_cpu_to_le64(next_layout.leaf_count);
    next_sb.generation = infs_cpu_to_le64(generation);
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        next_sb.checkpoint_block[i] =
            infs_cpu_to_le64(checkpoint[i]);

    uint8_t sb_block[INFS_BLOCK_SIZE];
    status = infs_encode_superblock(sb_block, &next_sb);
    if (status != INFS_STATUS_OK)
        goto out;

    /*
     * Block zero is the resize commit point because it also publishes the new
     * geometry. Replacement allocation pages are already durable and the old
     * tree has not been overwritten, so either side of this flush is a
     * complete mountable generation.
     */
    status = infs_storage_write(
        &vol->storage, 0, sb_block, sizeof(sb_block));
    if (status != INFS_STATUS_OK)
        goto out;
    status = infs_storage_flush(&vol->storage);
    if (status != INFS_STATUS_OK) {
        vol->reopen_required_status = status;
        vol->writable = 0;
        goto out;
    }

    free(vol->bitmap);
    vol->bitmap = next_bitmap;
    vol->bitmap_bytes = next_bitmap_bytes;
    next_bitmap = NULL;
    vol->sb = next_sb;
    allocation_map_cache_replace(vol, &next_layout);
    free_extent_index_destroy(vol);
    (void)free_extent_index_rebuild(vol);
    vol->data_allocation_cursor = 1u;
    vol->metadata_allocation_cursor = new_total;

    /*
     * The storage object keeps the physical backing size; the superblock is
     * the authoritative filesystem geometry.  Keep runtime validation bounded
     * by the new bitmap even when the partition/image is larger.
     */
    vol->checksum_cursor_valid = 0;
    object_cache_clear(vol);
    directory_cache_clear(vol);
    (void)object_cache_rebuild(vol);

    infs_status replica_status = INFS_STATUS_OK;
    for (unsigned i = 1; i < INFS_CHECKPOINT_COUNT; ++i) {
        status = infs_storage_write(
            &vol->storage, checkpoint[i] * INFS_BLOCK_SIZE,
            sb_block, sizeof(sb_block));
        if (status != INFS_STATUS_OK &&
            replica_status == INFS_STATUS_OK)
            replica_status = status;
    }
    status = infs_storage_flush(&vol->storage);
    if (status != INFS_STATUS_OK &&
        replica_status == INFS_STATUS_OK)
        replica_status = status;
    vol->checkpoint_repair_needed =
        replica_status != INFS_STATUS_OK;
    status = INFS_STATUS_OK;

out:
    allocation_map_layout_destroy(&next_layout);
    free(next_bitmap);
    return status;
}
