// SPDX-License-Identifier: GPL-3.0-or-later
static infs_status require_checkpoint_recovery(struct infs_volume *vol,
                                               infs_status status)
{
    if (status == INFS_STATUS_OK)
        status = INFS_STATUS_IO_ERROR;
    vol->reopen_required_status = status;
    vol->writable = 0;
    vol->checkpoint_repair_needed = 0;
    vol->tx_pending_bytes = 0;
    tx_abort(vol);
    return status;
}

infs_status infs_volume_sync(struct infs_volume *vol)
{
    if (!vol || !infs_storage_valid(&vol->storage))
        return INFS_STATUS_INVALID_ARGUMENT;
    if (vol->reopen_required_status != INFS_STATUS_OK)
        return vol->reopen_required_status;
    if (!vol->writable)
        return INFS_STATUS_OK;

    if (!vol->tx_active) {
        vol->tx_pending_bytes = 0;
        if (vol->checkpoint_repair_needed) {
            infs_status status = heal_checkpoints(vol);
            if (status != INFS_STATUS_OK)
                return status;
            vol->checkpoint_repair_needed = 0;
        }
        return infs_storage_flush(&vol->storage);
    }

    if (vol->tx_error != INFS_STATUS_OK)
        return vol->tx_error;

    uint64_t next_generation = 0;
    infs_status status = transaction_next_generation(vol, &next_generation);
    if (status != INFS_STATUS_OK)
        return status;

    const uint64_t old_bitmap_start =
        infs_le64_to_cpu(vol->tx_base_sb.bitmap_start_block);
    const uint64_t bitmap_blocks =
        infs_le64_to_cpu(vol->tx_base_sb.bitmap_block_count);
    status = tx_defer_range(vol, old_bitmap_start, bitmap_blocks);
    if (status != INFS_STATUS_OK)
        return status;

    uint64_t new_bitmap_start = 0;
    status = alloc_exact_run_uninitialized(
        vol, bitmap_blocks, &new_bitmap_start);
    if (status != INFS_STATUS_OK)
        return status;

    uint8_t *snapshot_union = NULL;
    status = snapshot_union_bitmap(vol, &snapshot_union);
    if (status != INFS_STATUS_OK)
        return status;

    if (vol->tx_error != INFS_STATUS_OK) {
        free(snapshot_union);
        return vol->tx_error;
    }

    /* Deferred frees remain reserved until every new metadata/data write is on
     * stable storage. They become reusable only in the new bitmap image. Keep
     * free_blocks incrementally instead of rescanning every block in a large
     * volume at each commit. */
    uint64_t free_blocks = infs_le64_to_cpu(vol->sb.free_blocks);
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    for (size_t i = 0; i < vol->tx_deferred_count; ++i) {
        uint64_t start = vol->tx_deferred[i].start;
        uint64_t count = vol->tx_deferred[i].count;
        for (uint64_t b = start; b < start + count && b < total; ++b) {
            if (!bitmap_get(vol->bitmap, b) ||
                bitmap_get(snapshot_union, b))
                continue;
            bitmap_set(vol->bitmap, b, 0);
            if (vol->free_extent_index_valid &&
                !free_extent_index_add(vol, b, 1))
                free_extent_index_invalidate(vol);
            if (free_blocks == UINT64_MAX) {
                free(snapshot_union);
                return INFS_STATUS_OVERFLOW;
            }
            ++free_blocks;
        }
    }
    for (uint64_t i = 0; i < bitmap_blocks; ++i)
        bitmap_set(vol->bitmap, new_bitmap_start + i, 1);
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        bitmap_set(vol->bitmap,
                   infs_le64_to_cpu(vol->sb.checkpoint_block[i]), 1);
    for (uint64_t b = total; b < (uint64_t)vol->bitmap_bytes * 8u; ++b)
        bitmap_set(vol->bitmap, b, 1);
    free(snapshot_union);

    vol->sb.bitmap_start_block = infs_cpu_to_le64(new_bitmap_start);
    vol->sb.generation = infs_cpu_to_le64(next_generation);
    vol->sb.free_blocks = infs_cpu_to_le64(free_blocks);

    /* Ordered transaction protocol:
     *   1. new metadata/data already written to unreachable blocks;
     *   2. flush those blocks;
     *   3. write and flush a new bitmap image;
     *   4. publish one checksummed checkpoint (commit point);
     *   5. replicate that checkpoint to the other physical copies.
     * A crash before step 4 selects the old generation. A crash after step 4
     * selects the new generation and a writable open heals the other copies. */
    status = infs_storage_flush(&vol->storage);
    if (status != INFS_STATUS_OK)
        return status;
    phase2_failpoint("before-bitmap");

    status = infs_storage_write(&vol->storage,
                                new_bitmap_start * INFS_BLOCK_SIZE,
                                vol->bitmap, vol->bitmap_bytes);
    if (status != INFS_STATUS_OK)
        return status;
    status = infs_storage_flush(&vol->storage);
    if (status != INFS_STATUS_OK)
        return status;
    phase2_failpoint("after-bitmap");

    uint8_t sb_block[INFS_BLOCK_SIZE];
    status = infs_encode_superblock(sb_block, &vol->sb);
    if (status != INFS_STATUS_OK)
        return status;
    uint64_t generation = infs_le64_to_cpu(vol->sb.generation);
    unsigned first = (unsigned)(generation % INFS_CHECKPOINT_COUNT);
    uint64_t first_block =
        infs_le64_to_cpu(vol->sb.checkpoint_block[first]);
    status = write_block(vol, first_block, sb_block);
    if (status != INFS_STATUS_OK)
        return require_checkpoint_recovery(vol, status);
    status = infs_storage_flush(&vol->storage);
    if (status != INFS_STATUS_OK)
        return require_checkpoint_recovery(vol, status);

    /* The transaction is durably committed from this point onward. A failure
     * while replicating secondary checkpoints must not be reported to callers
     * as if the namespace/data mutation itself failed. Track the degraded
     * redundancy state and let fsync/sync heal it explicitly. */
    free(vol->tx_base_bitmap);
    vol->tx_base_bitmap = NULL;
    vol->tx_deferred_count = 0;
    vol->tx_error = INFS_STATUS_OK;
    vol->tx_active = 0;
    vol->tx_pending_bytes = 0;
    phase2_failpoint("after-checkpoint");

    infs_status replica_status = INFS_STATUS_OK;
    for (unsigned n = 1; n < INFS_CHECKPOINT_COUNT; ++n) {
        unsigned i = (first + n) % INFS_CHECKPOINT_COUNT;
        uint64_t b = infs_le64_to_cpu(vol->sb.checkpoint_block[i]);
        status = write_block(vol, b, sb_block);
        if (status != INFS_STATUS_OK && replica_status == INFS_STATUS_OK)
            replica_status = status;
    }
    status = infs_storage_flush(&vol->storage);
    if (status != INFS_STATUS_OK && replica_status == INFS_STATUS_OK)
        replica_status = status;

    if (replica_status != INFS_STATUS_OK) {
        vol->checkpoint_repair_needed = 1;
        return INFS_STATUS_OK;
    }

    vol->checkpoint_repair_needed = 0;
    return INFS_STATUS_OK;
}

infs_status infs_volume_set_deferred_publish(struct infs_volume *vol,
                                             int enabled,
                                             uint64_t threshold_bytes)
{
    if (!vol || !infs_storage_valid(&vol->storage))
        return INFS_STATUS_INVALID_ARGUMENT;
    if (enabled && !vol->writable)
        return INFS_STATUS_READ_ONLY;

    if (!enabled && vol->deferred_publish && vol->tx_active) {
        infs_status status = infs_volume_sync(vol);
        if (status != INFS_STATUS_OK)
            return status;
    }

    vol->deferred_publish = enabled != 0;
    vol->deferred_publish_threshold_bytes = enabled ?
        (threshold_bytes ? threshold_bytes :
         INFS_DEFAULT_DEFERRED_PUBLISH_BYTES) : 0;
    if (!vol->tx_active)
        vol->tx_pending_bytes = 0;
    return INFS_STATUS_OK;
}

static infs_status finish_mutation(struct infs_volume *vol,
                                   uint64_t approximate_dirty_bytes)
{
    if (!vol || !vol->tx_active)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (vol->tx_error != INFS_STATUS_OK)
        return vol->tx_error;

    vol->tx_pending_bytes = infiltratr_u64_add_saturating(
        vol->tx_pending_bytes, approximate_dirty_bytes);

    if (!vol->deferred_publish)
        return infs_volume_sync(vol);

    uint64_t threshold = vol->deferred_publish_threshold_bytes;
    if (!threshold)
        threshold = INFS_DEFAULT_DEFERRED_PUBLISH_BYTES;
    if (vol->tx_pending_bytes >= threshold)
        return infs_volume_sync(vol);
    return INFS_STATUS_OK;
}
