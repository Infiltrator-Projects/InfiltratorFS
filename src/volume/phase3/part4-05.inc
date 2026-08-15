// SPDX-License-Identifier: GPL-3.0-or-later
static int punch_partial_block(struct infs_volume *vol,
                               uint8_t object[INFS_BLOCK_SIZE],
                               struct infs_file_payload_disk *file,
                               struct infs_extent_disk *ext,
                               uint64_t logical, size_t within, size_t length)
{
    uint64_t physical;
    uint32_t flags;
    if (file_map_block(file, ext, logical, &physical, &flags) != 0)
        return INFS_STATUS_CORRUPT;
    if (flags == INFS_EXTENT_HOLE)
        return 0;

    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    uint8_t data[INFS_BLOCK_SIZE];
    if (read_block(vol, physical, data) != 0 ||
        data_block_verify(vol, file, hdr->object_id, logical, data) != 0)
        return INFS_STATUS_ERROR;
    memset(data + within, 0, length);

    uint64_t replacement = 0, got = 0;
    int status = alloc_run(vol, 1, &replacement, &got);
    if (status != 0)
        return status;
    if (got != 1)
        return INFS_STATUS_CORRUPT;
    if (write_block(vol, replacement, data) != 0 ||
        file_replace_range(vol, object, logical, 1, replacement,
                           INFS_EXTENT_NORMAL) != 0)
        return INFS_STATUS_ERROR;

    struct infs_data_checksum_disk digest;
    data_block_checksum(data, &digest);
    return checksum_set_range(vol, file, hdr->object_id, logical, &digest, 1);
}

static int punch_hole_tx(struct infs_volume *vol, const char *path,
                         uint64_t offset, uint64_t length)
{
    struct infs_lookup lk;
    int status = infs_lookup_path(vol, path, &lk);
    if (status != 0)
        return status;
    if (lk.type != INFS_OBJECT_FILE)
        return INFS_STATUS_IS_DIRECTORY;

    uint8_t object[INFS_BLOCK_SIZE];
    if (read_object(vol, lk.block, object) != 0)
        return INFS_STATUS_ERROR;
    struct infs_file_payload_disk *file;
    struct infs_extent_disk *ext;
    if (file_validate_volume(vol, object, &file, &ext) != 0)
        return INFS_STATUS_ERROR;

    uint64_t file_size = infs_le64_to_cpu(file->attributes.logical_size);
    uint64_t logical_end = file_size;
    uint64_t tail = file_size % INFS_BLOCK_SIZE;
    if (tail) {
        uint64_t padding = INFS_BLOCK_SIZE - tail;
        logical_end = file_size > UINT64_MAX - padding ?
            UINT64_MAX : file_size + padding;
    }
    uint64_t end = offset + length;
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
        if (status != 0)
            return status;
        pos += chunk;
    }

    uint64_t full_blocks = (end - pos) / INFS_BLOCK_SIZE;
    while (full_blocks) {
        uint32_t chunk = full_blocks > UINT32_MAX ?
            UINT32_MAX : (uint32_t)full_blocks;
        status = file_replace_range(vol, object, pos / INFS_BLOCK_SIZE,
                                    chunk, 0, INFS_EXTENT_HOLE);
        if (status != 0)
            return status;
        pos += (uint64_t)chunk * INFS_BLOCK_SIZE;
        full_blocks -= chunk;
    }

    if (pos < end) {
        status = punch_partial_block(vol, object, file, ext,
                                     pos / INFS_BLOCK_SIZE, 0,
                                     (size_t)(end - pos));
        if (status != 0)
            return status;
    }

    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    if (file_allocated_blocks(file, ext) == 0 &&
        !id_is_zero(file->checksum_head_id)) {
        status = checksum_destroy_chain(vol, file, hdr->object_id);
        if (status != 0)
            return status;
    }
    touch_mctime(vol, &file->attributes);
    if (write_object(vol, lk.block, object) != 0)
        return INFS_STATUS_ERROR;
    return infs_volume_sync(vol);
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

    status = tx_begin(vol);
    if (status != INFS_STATUS_OK)
        return status;
    status = punch_hole_tx(vol, path, offset, length);
    if (status != INFS_STATUS_OK)
        tx_abort(vol);
    return status;
}
