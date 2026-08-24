// SPDX-License-Identifier: GPL-3.0-or-later
static int punch_partial_block(struct infs_volume *vol,
                               uint8_t object[INFS_BLOCK_SIZE],
                               struct infs_file_payload_disk *file,
                               struct infs_extent_disk *ext,
                               uint64_t logical, size_t within, size_t length)
{
    uint64_t physical;
    uint32_t flags;
    infs_status status = file_map_block(vol, object, file, ext, logical, &physical, &flags);
    if (status != INFS_STATUS_OK)
        return status;
    if (flags == INFS_EXTENT_HOLE)
        return INFS_STATUS_OK;

    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    uint8_t data[INFS_BLOCK_SIZE];
    status = read_block(vol, physical, data);
    if (status != INFS_STATUS_OK)
        return status;
    status = data_block_verify(vol, file, hdr->object_id, logical, data);
    if (status != INFS_STATUS_OK)
        return status;
    memset(data + within, 0, length);

    uint64_t replacement = 0, got = 0;
    status = alloc_data_run(vol, 1, &replacement, &got);
    if (status != INFS_STATUS_OK)
        return status;
    if (got != 1)
        return INFS_STATUS_CORRUPT;
    status = write_block(vol, replacement, data);
    if (status != INFS_STATUS_OK)
        return status;
    status = file_replace_range(vol, object, logical, 1, replacement,
                                INFS_EXTENT_NORMAL);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_data_checksum_disk digest;
    data_block_checksum(data, &digest);
    return checksum_set_range(vol, file, hdr->object_id, logical, &digest, 1);
}

static int punch_hole_tx(struct infs_volume *vol, const char *path,
                         uint64_t offset, uint64_t length)
{
    struct infs_lookup lk;
    infs_status status = infs_lookup_path(vol, path, &lk);
    if (status != INFS_STATUS_OK)
        return status;
    if (lk.type != INFS_OBJECT_FILE)
        return INFS_STATUS_IS_DIRECTORY;

    uint8_t object[INFS_BLOCK_SIZE];
    status = read_object(vol, lk.block, object);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_file_payload_disk *file;
    struct infs_extent_disk *ext;
    status = file_validate_volume(vol, object, &file, &ext);
    if (status != INFS_STATUS_OK)
        return status;

    uint64_t file_size = infs_le64_to_cpu(file->attributes.logical_size);
    uint64_t end = offset + length;
    if (end > file_size)
        end = file_size;

    if (file_is_inline(object, file)) {
        uint8_t inline_data[INFS_INLINE_DATA_MAX];
        status = file_read_small_content(vol, object, inline_data);
        if (status != INFS_STATUS_OK)
            return status;
        memset(inline_data + (size_t)offset, 0, (size_t)(end - offset));
        status = file_store_inline(vol, object, inline_data, (size_t)file_size);
        if (status != INFS_STATUS_OK)
            return status;
        file = (struct infs_file_payload_disk *)(
            object + sizeof(struct infs_object_header_disk));
        touch_mctime(vol, &file->attributes);
        if (vol->tx_error != INFS_STATUS_OK)
            return vol->tx_error;
        status = infs_object_finalize(object);
        if (status != INFS_STATUS_OK)
            return status;
        status = file_validate_volume(vol, object, &file, &ext);
        if (status != INFS_STATUS_OK)
            return status;
        status = write_object(vol, lk.block, object);
        if (status != INFS_STATUS_OK)
            return status;
        return finish_mutation(vol, (uint64_t)INFS_BLOCK_SIZE * 2u);
    }

    uint64_t logical_end = file_size;
    uint64_t tail = file_size % INFS_BLOCK_SIZE;
    if (tail) {
        uint64_t padding = INFS_BLOCK_SIZE - tail;
        logical_end = file_size > UINT64_MAX - padding ?
            UINT64_MAX : file_size + padding;
    }
    end = offset + length;
    if (end > logical_end)
        end = logical_end;
    uint64_t pos = offset;

    size_t within = (size_t)(pos % INFS_BLOCK_SIZE);
    if (within) {
        size_t chunk = INFS_BLOCK_SIZE - within;
        if ((uint64_t)chunk > end - pos)
            chunk = (size_t)(end - pos);
        status = punch_partial_block(vol, object, file, ext,
                                     pos / INFS_BLOCK_SIZE, within, chunk);
        if (status != INFS_STATUS_OK)
            return status;
        pos += chunk;
    }

    uint64_t full_blocks = (end - pos) / INFS_BLOCK_SIZE;
    while (full_blocks) {
        uint32_t chunk = full_blocks > UINT32_MAX ?
            UINT32_MAX : (uint32_t)full_blocks;
        status = file_replace_range(vol, object, pos / INFS_BLOCK_SIZE,
                                    chunk, 0, INFS_EXTENT_HOLE);
        if (status != INFS_STATUS_OK)
            return status;
        pos += (uint64_t)chunk * INFS_BLOCK_SIZE;
        full_blocks -= chunk;
    }

    if (pos < end) {
        status = punch_partial_block(vol, object, file, ext,
                                     pos / INFS_BLOCK_SIZE, 0,
                                     (size_t)(end - pos));
        if (status != INFS_STATUS_OK)
            return status;
    }

    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    uint64_t allocated_blocks = 0;
    status = file_allocated_blocks_volume(
        vol, object, file, ext, &allocated_blocks);
    if (status != INFS_STATUS_OK)
        return status;
    if (allocated_blocks == 0 && !id_is_zero(file->checksum_head_id)) {
        status = checksum_destroy_chain(vol, file, hdr->object_id);
        if (status != INFS_STATUS_OK)
            return status;
    }
    touch_mctime(vol, &file->attributes);
    if (vol->tx_error != INFS_STATUS_OK)
        return vol->tx_error;
    status = write_object(vol, lk.block, object);
    if (status != INFS_STATUS_OK)
        return status;
    return finish_mutation(vol, length + INFS_BLOCK_SIZE);
}

infs_status infs_punch_hole(struct infs_volume *vol, const char *path,
                            uint64_t offset, uint64_t length)
{
    if (!vol || !path)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (length == 0)
        return INFS_STATUS_OK;
    if (offset > UINT64_MAX - length)
        return INFS_STATUS_OVERFLOW;

    struct infs_attributes attributes;
    infs_status status = infs_get_attributes(vol, path, &attributes);
    if (status != INFS_STATUS_OK)
        return status;
    if (attributes.object_type != INFS_OBJECT_FILE)
        return INFS_STATUS_IS_DIRECTORY;
    if (offset >= attributes.logical_size)
        return INFS_STATUS_OK;

    int started_transaction = 0;
    status = mutation_scope_begin(vol, &started_transaction);
    if (status != INFS_STATUS_OK)
        return status;
    status = punch_hole_tx(vol, path, offset, length);
    if (status != INFS_STATUS_OK)
        mutation_scope_fail(vol, started_transaction);
    else
        mutation_scope_success(vol);
    return status;
}
