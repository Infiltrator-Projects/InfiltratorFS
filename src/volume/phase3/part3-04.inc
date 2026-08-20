// SPDX-License-Identifier: GPL-3.0-or-later
static void discard_candidate_state(struct infs_volume *vol)
{
    free(vol->bitmap);
    vol->bitmap = NULL;
    vol->bitmap_bytes = 0;
}

static infs_status checkpoint_graph_status(infs_status status)
{
    if (status == INFS_STATUS_NOT_FOUND ||
        status == INFS_STATUS_LOOP_DETECTED)
        return INFS_STATUS_CORRUPT;
    return status;
}

static infs_status read_checkpoint_candidates(
    struct infs_volume *vol,
    struct infs_superblock_disk candidates[INFS_CHECKPOINT_COUNT],
    uint8_t valid[INFS_CHECKPOINT_COUNT])
{
    if (!vol || !candidates || !valid || vol->size_bytes < INFS_BLOCK_SIZE * 3u)
        return INFS_STATUS_INVALID_ARGUMENT;

    memset(valid, 0, INFS_CHECKPOINT_COUNT);
    const uint64_t total_blocks = vol->size_bytes / INFS_BLOCK_SIZE;
    const uint64_t blocks[INFS_CHECKPOINT_COUNT] = {
        0,
        total_blocks / 2u,
        total_blocks - 1u
    };
    unsigned found = 0;
    infs_status first_read_error = INFS_STATUS_OK;
    uint8_t raw[INFS_BLOCK_SIZE];

    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        infs_status status = infs_storage_read(
            &vol->storage, blocks[i] * INFS_BLOCK_SIZE, raw, sizeof(raw));
        if (status != INFS_STATUS_OK) {
            if (first_read_error == INFS_STATUS_OK)
                first_read_error = status;
            continue;
        }
        if (!infs_validate_superblock_block(raw))
            continue;

        struct infs_superblock_disk sb;
        memcpy(&sb, raw, sizeof(sb));
        if (infs_le64_to_cpu(sb.total_blocks) != total_blocks ||
            infs_le64_to_cpu(sb.checkpoint_block[0]) != blocks[0] ||
            infs_le64_to_cpu(sb.checkpoint_block[1]) != blocks[1] ||
            infs_le64_to_cpu(sb.checkpoint_block[2]) != blocks[2])
            continue;

        candidates[i] = sb;
        valid[i] = 1;
        ++found;
    }

    /* A read-only opener can inspect any surviving replica. A writable opener
     * must not heal over an unreadable location: that location may hold the
     * only durably committed newer generation after a crash between primary
     * checkpoint publication and replication. */
    if (first_read_error != INFS_STATUS_OK && vol->writable)
        return first_read_error;
    if (found)
        return INFS_STATUS_OK;
    return first_read_error != INFS_STATUS_OK ?
        first_read_error : INFS_STATUS_CORRUPT;
}

static infs_status load_checkpoint_graph(
    struct infs_volume *vol, const struct infs_superblock_disk *candidate)
{
    if (!vol || !candidate)
        return INFS_STATUS_INVALID_ARGUMENT;

    discard_candidate_state(vol);
    vol->sb = *candidate;

    if (infs_le16_to_cpu(vol->sb.format_minor) != INFS_FORMAT_MINOR)
        return INFS_STATUS_NOT_SUPPORTED;
    if (vol->writable &&
        (infs_le64_to_cpu(vol->sb.ro_compat_flags) &
         ~INFS_KNOWN_RO_COMPAT_FLAGS) != 0)
        return INFS_STATUS_NOT_SUPPORTED;

    uint64_t bitmap_start = infs_le64_to_cpu(vol->sb.bitmap_start_block);
    uint64_t bitmap_blocks = infs_le64_to_cpu(vol->sb.bitmap_block_count);
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    const uint64_t bits_per_bitmap_block =
        (uint64_t)INFS_BLOCK_SIZE * UINT64_C(8);
    uint64_t minimum_bitmap_blocks = total / bits_per_bitmap_block +
        ((total % bits_per_bitmap_block) != 0);
    if (bitmap_start == 0 || bitmap_blocks == 0 || bitmap_start >= total ||
        bitmap_blocks > total - bitmap_start ||
        bitmap_blocks < minimum_bitmap_blocks ||
        bitmap_blocks > SIZE_MAX / INFS_BLOCK_SIZE ||
        infs_le64_to_cpu(vol->sb.free_blocks) > total)
        return INFS_STATUS_CORRUPT;

    vol->bitmap_bytes = (size_t)(bitmap_blocks * INFS_BLOCK_SIZE);
    vol->bitmap = malloc(vol->bitmap_bytes);
    if (!vol->bitmap)
        return INFS_STATUS_NO_MEMORY;

    infs_status status = infs_storage_read(
        &vol->storage, bitmap_start * INFS_BLOCK_SIZE,
        vol->bitmap, vol->bitmap_bytes);
    if (status != INFS_STATUS_OK)
        return status;

    uint64_t free_count = 0;
    for (uint64_t b = 0; b < total; ++b) {
        if (!bitmap_get(vol->bitmap, b))
            ++free_count;
    }
    if (free_count != infs_le64_to_cpu(vol->sb.free_blocks))
        return INFS_STATUS_CORRUPT;
    for (uint64_t b = total; b < (uint64_t)vol->bitmap_bytes * 8u; ++b) {
        if (!bitmap_get(vol->bitmap, b))
            return INFS_STATUS_CORRUPT;
    }
    for (uint64_t b = bitmap_start; b < bitmap_start + bitmap_blocks; ++b) {
        if (!bitmap_get(vol->bitmap, b))
            return INFS_STATUS_CORRUPT;
    }

    uint64_t critical[] = {
        infs_le64_to_cpu(vol->sb.checkpoint_block[0]),
        infs_le64_to_cpu(vol->sb.checkpoint_block[1]),
        infs_le64_to_cpu(vol->sb.checkpoint_block[2]),
        infs_le64_to_cpu(vol->sb.object_index_block),
        infs_le64_to_cpu(vol->sb.root_object_block)
    };
    for (size_t i = 0; i < sizeof(critical) / sizeof(critical[0]); ++i) {
        if (critical[i] >= total || !bitmap_get(vol->bitmap, critical[i]))
            return INFS_STATUS_CORRUPT;
    }

    uint8_t object[INFS_BLOCK_SIZE];
    status = read_object(vol, infs_le64_to_cpu(vol->sb.root_object_block), object);
    if (status != INFS_STATUS_OK)
        return checkpoint_graph_status(status);
    status = dir_validate(object, NULL);
    if (status != INFS_STATUS_OK)
        return checkpoint_graph_status(status);
    struct infs_object_header_disk *root_hdr =
        (struct infs_object_header_disk *)object;
    if (memcmp(root_hdr->object_id, vol->sb.root_object_id, 16) != 0 ||
        !id_is_zero(root_hdr->parent_id))
        return INFS_STATUS_CORRUPT;

    status = validate_index_entries(vol);
    if (status != INFS_STATUS_OK)
        return checkpoint_graph_status(status);
    status = validate_integrity_metadata(vol);
    if (status != INFS_STATUS_OK)
        return checkpoint_graph_status(status);
    status = validate_namespace_graph(vol);
    if (status != INFS_STATUS_OK)
        return checkpoint_graph_status(status);
    return checkpoint_graph_status(validate_checksum_graph(vol));
}

infs_status infs_volume_open_storage(struct infs_volume *vol,
                                     struct infs_storage *storage, int writable)
{
    if (!vol || !storage || !infs_storage_valid(storage))
        return INFS_STATUS_INVALID_ARGUMENT;
    memset(vol, 0, sizeof(*vol));
    vol->writable = writable != 0;
    vol->storage = *storage;
    storage->ops = NULL;
    storage->context = NULL;

    infs_status status = INFS_STATUS_OK;
    if (vol->writable &&
        (!vol->storage.ops->write_at || !vol->storage.ops->flush ||
         !vol->storage.ops->random_bytes ||
         !vol->storage.ops->current_time_ns)) {
        status = INFS_STATUS_INVALID_ARGUMENT;
        goto fail;
    }

    int is_block = 0;
    status = infs_storage_get_size(&vol->storage, &vol->size_bytes, &is_block);
    if (status != INFS_STATUS_OK)
        goto fail;
    (void)is_block;

    struct infs_superblock_disk candidates[INFS_CHECKPOINT_COUNT];
    uint8_t valid[INFS_CHECKPOINT_COUNT];
    status = read_checkpoint_candidates(vol, candidates, valid);
    if (status != INFS_STATUS_OK)
        goto fail;

    int selected = 0;
    for (unsigned attempt = 0; attempt < INFS_CHECKPOINT_COUNT; ++attempt) {
        int best = -1;
        uint64_t best_generation = 0;
        for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
            if (!valid[i])
                continue;
            uint64_t generation = infs_le64_to_cpu(candidates[i].generation);
            if (best < 0 || generation > best_generation) {
                best = (int)i;
                best_generation = generation;
            }
        }
        if (best < 0)
            break;

        valid[best] = 0;
        status = load_checkpoint_graph(vol, &candidates[best]);
        if (status == INFS_STATUS_OK) {
            selected = 1;
            break;
        }

        discard_candidate_state(vol);
        if (status != INFS_STATUS_CORRUPT)
            goto fail;
    }

    if (!selected) {
        status = INFS_STATUS_CORRUPT;
        goto fail;
    }

    if (vol->writable) {
        status = heal_checkpoints(vol);
        if (status != INFS_STATUS_OK)
            goto fail;
    }
    return INFS_STATUS_OK;

fail:
    discard_candidate_state(vol);
    infs_storage_close(&vol->storage);
    return status == INFS_STATUS_OK ? INFS_STATUS_ERROR : status;
}

void infs_volume_close(struct infs_volume *vol)
{
    if (!vol)
        return;
    tx_abort(vol);
    free(vol->tx_base_bitmap);
    vol->tx_base_bitmap = NULL;
    free(vol->tx_deferred);
    vol->tx_deferred = NULL;
    vol->tx_deferred_count = 0;
    vol->tx_deferred_capacity = 0;
    discard_candidate_state(vol);
    infs_storage_close(&vol->storage);
}
