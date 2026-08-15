// SPDX-License-Identifier: GPL-3.0-or-later
infs_status infs_volume_open_storage(struct infs_volume *vol,
                                     struct infs_storage *storage, int writable)
{
    if (!vol || !storage || !infs_storage_valid(storage)) {
        return INFS_STATUS_INVALID_ARGUMENT;
    }
    memset(vol, 0, sizeof(*vol));
    vol->writable = writable != 0;
    vol->storage = *storage;
    storage->ops = NULL;
    storage->context = NULL;
    int is_block = 0;
    infs_status status = infs_storage_get_size(
        &vol->storage, &vol->size_bytes, &is_block);
    if (status != INFS_STATUS_OK)
        goto fail;
    (void)is_block;
    status = infs_read_best_superblock(&vol->storage, vol->size_bytes,
                                       &vol->sb, NULL);
    if (status != INFS_STATUS_OK)
        goto fail;
    if (infs_le16_to_cpu(vol->sb.format_minor) != INFS_FORMAT_MINOR) {
        status = INFS_STATUS_NOT_SUPPORTED;
        goto fail;
    }

    uint64_t bitmap_start = infs_le64_to_cpu(vol->sb.bitmap_start_block);
    uint64_t bitmap_blocks = infs_le64_to_cpu(vol->sb.bitmap_block_count);
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    if (bitmap_start == 0 || bitmap_blocks == 0 || bitmap_start >= total ||
        bitmap_blocks > total - bitmap_start ||
        bitmap_blocks > SIZE_MAX / INFS_BLOCK_SIZE) {
        status = INFS_STATUS_CORRUPT;
        goto fail;
    }
    vol->bitmap_bytes = (size_t)(bitmap_blocks * INFS_BLOCK_SIZE);
    vol->bitmap = malloc(vol->bitmap_bytes);
    if (!vol->bitmap) {
        status = INFS_STATUS_NO_MEMORY;
        goto fail;
    }
    status = infs_storage_read(&vol->storage,
                               bitmap_start * INFS_BLOCK_SIZE,
                               vol->bitmap, vol->bitmap_bytes);
    if (status != INFS_STATUS_OK)
        goto fail;

    uint64_t free_count = 0;
    for (uint64_t b = 0; b < total; ++b) {
        if (!bitmap_get(vol->bitmap, b))
            ++free_count;
    }
    if (free_count != infs_le64_to_cpu(vol->sb.free_blocks)) {
        status = INFS_STATUS_CORRUPT;
        goto fail;
    }
    for (uint64_t b = total; b < (uint64_t)vol->bitmap_bytes * 8u; ++b) {
        if (!bitmap_get(vol->bitmap, b)) {
            status = INFS_STATUS_CORRUPT;
            goto fail;
        }
    }
    for (uint64_t b = bitmap_start; b < bitmap_start + bitmap_blocks; ++b) {
        if (!bitmap_get(vol->bitmap, b)) {
            status = INFS_STATUS_CORRUPT;
            goto fail;
        }
    }

    uint64_t critical[] = {
        infs_le64_to_cpu(vol->sb.checkpoint_block[0]),
        infs_le64_to_cpu(vol->sb.checkpoint_block[1]),
        infs_le64_to_cpu(vol->sb.checkpoint_block[2]),
        infs_le64_to_cpu(vol->sb.object_index_block),
        infs_le64_to_cpu(vol->sb.root_object_block)
    };
    for (size_t i = 0; i < sizeof(critical) / sizeof(critical[0]); ++i) {
        if (critical[i] >= total || !bitmap_get(vol->bitmap, critical[i])) {
            status = INFS_STATUS_CORRUPT;
            goto fail;
        }
    }

    uint8_t object[INFS_BLOCK_SIZE];
    status = read_object(vol, infs_le64_to_cpu(vol->sb.root_object_block), object);
    if (status != INFS_STATUS_OK)
        goto fail;
    status = dir_validate(object, NULL);
    if (status != INFS_STATUS_OK)
        goto fail;
    struct infs_object_header_disk *root_hdr = (struct infs_object_header_disk *)object;
    if (memcmp(root_hdr->object_id, vol->sb.root_object_id, 16) != 0 ||
        !id_is_zero(root_hdr->parent_id)) {
        status = INFS_STATUS_CORRUPT;
        goto fail;
    }
    status = validate_index_entries(vol);
    if (status != INFS_STATUS_OK)
        goto fail;
    status = validate_integrity_metadata(vol);
    if (status != INFS_STATUS_OK)
        goto fail;
    if (vol->writable) {
        status = heal_checkpoints(vol);
        if (status != INFS_STATUS_OK)
            goto fail;
    }
    return INFS_STATUS_OK;

fail:
    free(vol->bitmap);
    vol->bitmap = NULL;
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
    free(vol->bitmap);
    vol->bitmap = NULL;
    infs_storage_close(&vol->storage);
}
