// SPDX-License-Identifier: GPL-3.0-or-later
static infs_status abort_precheckpoint_transaction(
    struct infs_volume *vol, infs_status status)
{
    /*
     * No checkpoint has been attempted yet, so the committed generation is
     * still authoritative. Discard transaction-private allocations/deferred
     * frees immediately instead of leaving a half-published allocation map in
     * the live transaction for a later retry.
     */
    tx_abort(vol);
    vol->tx_pending_bytes = 0;
    return status == INFS_STATUS_OK ? INFS_STATUS_IO_ERROR : status;
}

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
    struct infs_allocation_layout next_allocation = {0};
    infs_status status = transaction_next_generation(vol, &next_generation);
    if (status != INFS_STATUS_OK)
        return status;

    /*
     * New data/metadata were written to unreachable blocks above. Flush them
     * first, then publish only the allocation leaves whose bit payload changed
     * plus the small branch spine. The checkpoint remains the atomic commit
     * point exactly as before.
     */
    status = infs_storage_flush(&vol->storage);
    if (status != INFS_STATUS_OK)
        return abort_precheckpoint_transaction(vol, status);
    phase2_failpoint("before-bitmap");

    status = allocation_map_publish(
        vol, next_generation, &next_allocation);
    if (status != INFS_STATUS_OK) {
        allocation_map_layout_destroy(&next_allocation);
        return abort_precheckpoint_transaction(vol, status);
    }
    status = infs_storage_flush(&vol->storage);
    if (status != INFS_STATUS_OK) {
        allocation_map_layout_destroy(&next_allocation);
        return abort_precheckpoint_transaction(vol, status);
    }
    phase2_failpoint("after-bitmap");

    uint8_t sb_block[INFS_BLOCK_SIZE];
    status = infs_encode_superblock(sb_block, &vol->sb);
    if (status != INFS_STATUS_OK) {
        allocation_map_layout_destroy(&next_allocation);
        return abort_precheckpoint_transaction(vol, status);
    }
    uint64_t generation = infs_le64_to_cpu(vol->sb.generation);
    unsigned first = (unsigned)(generation % INFS_CHECKPOINT_COUNT);
    uint64_t first_block =
        infs_le64_to_cpu(vol->sb.checkpoint_block[first]);
    status = write_block(vol, first_block, sb_block);
    if (status != INFS_STATUS_OK) {
        allocation_map_layout_destroy(&next_allocation);
        return require_checkpoint_recovery(vol, status);
    }
    status = infs_storage_flush(&vol->storage);
    if (status != INFS_STATUS_OK) {
        allocation_map_layout_destroy(&next_allocation);
        return require_checkpoint_recovery(vol, status);
    }

    allocation_map_cache_replace(vol, &next_allocation);

    /* The transaction is durably committed from this point onward. A failure
     * while replicating secondary checkpoints must not be reported to callers
     * as if the namespace/data mutation itself failed. Track the degraded
     * redundancy state and let fsync/sync heal it explicitly. */
    vol->tx_allocated_count = 0;
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
