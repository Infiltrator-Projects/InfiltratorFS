// SPDX-License-Identifier: GPL-3.0-or-later
static int64_t write_file_tx(struct infs_volume *vol, const char *path,
                             const void *buf, size_t size, uint64_t offset)
{
    if (!vol || !path || (size && !buf))
        return INFS_STATUS_INVALID_ARGUMENT;
    if (!vol->writable)
        return INFS_STATUS_READ_ONLY;
    if ((uint64_t)size > INT64_MAX || offset > UINT64_MAX - size)
        return INFS_STATUS_OVERFLOW;
    if (size == 0)
        return 0;

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
    struct infs_file_payload_disk *p;
    struct infs_extent_disk *ext;
    status = file_validate_volume(vol, object, &p, &ext);
    if (status != INFS_STATUS_OK)
        return status;

    uint64_t end = offset + size;
    uint64_t needed_blocks = end / INFS_BLOCK_SIZE +
        ((end % INFS_BLOCK_SIZE) != 0);
    status = file_ensure_logical_blocks(vol, object, needed_blocks);
    if (status != INFS_STATUS_OK)
        return status;
    if (end > infs_le64_to_cpu(p->attributes.logical_size))
        p->attributes.logical_size = infs_cpu_to_le64(end);

    status = infs_object_finalize(object);
    if (status != INFS_STATUS_OK)
        return status;
    status = file_validate_volume(vol, object, &p, &ext);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_object_header_disk *file_hdr =
        (struct infs_object_header_disk *)object;
    size_t done = 0;
    uint8_t data[INFS_BLOCK_SIZE];
    while (done < size) {
        uint64_t pos = offset + done;
        uint64_t logical = pos / INFS_BLOCK_SIZE;
        size_t within = (size_t)(pos % INFS_BLOCK_SIZE);
        size_t chunk = INFS_BLOCK_SIZE - within;
        if (chunk > size - done)
            chunk = size - done;

        uint64_t physical;
        uint32_t flags;
        status = file_map_block(p, ext, logical, &physical, &flags);
        if (status != INFS_STATUS_OK)
            return status;

        int full_overwrite = within == 0 && chunk == INFS_BLOCK_SIZE;
        if (flags == INFS_EXTENT_HOLE || full_overwrite) {
            memset(data, 0, sizeof(data));
        } else {
            status = read_block(vol, physical, data);
            if (status != INFS_STATUS_OK)
                return status;
            status = data_block_verify(vol, p, file_hdr->object_id, logical,
                                       data);
            if (status != INFS_STATUS_OK)
                return status;
        }
        memcpy(data + within, (const uint8_t *)buf + done, chunk);

        /* A block that existed in generation N is never overwritten. Replace
         * it with an unreachable block and repoint the extent metadata. Blocks
         * allocated earlier in this same transaction are already unreachable
         * from N and may be filled in place before publication. */
        if (flags == INFS_EXTENT_HOLE ||
            bitmap_get(vol->tx_base_bitmap, physical)) {
            uint64_t replacement = 0, got = 0;
            status = alloc_run(vol, 1, &replacement, &got);
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
            physical = replacement;
        } else {
            status = write_block(vol, physical, data);
            if (status != INFS_STATUS_OK)
                return status;
        }

        struct infs_data_checksum_disk digest;
        data_block_checksum(data, &digest);
        status = checksum_set_range(vol, p, file_hdr->object_id, logical,
                                    &digest, 1);
        if (status != INFS_STATUS_OK)
            return status;
        done += chunk;
    }

    touch_mctime(vol, &p->attributes);
    if (vol->tx_error != INFS_STATUS_OK)
        return vol->tx_error;
    status = write_object(vol, lk.block, object);
    if (status != INFS_STATUS_OK)
        return status;
    status = infs_volume_sync(vol);
    if (status != INFS_STATUS_OK)
        return status;
    return (int64_t)done;
}

int64_t infs_write_file(struct infs_volume *vol, const char *path,
                        const void *buf, size_t size, uint64_t offset)
{
    if (!vol || !path || (size && !buf))
        return INFS_STATUS_INVALID_ARGUMENT;
    if (!vol->writable)
        return INFS_STATUS_READ_ONLY;
    if ((uint64_t)size > INT64_MAX || offset > UINT64_MAX - size)
        return INFS_STATUS_OVERFLOW;
    if (size == 0)
        return 0;

    infs_status status = tx_begin(vol);
    if (status != INFS_STATUS_OK)
        return status;
    int64_t rc = write_file_tx(vol, path, buf, size, offset);
    if (rc < 0)
        tx_abort(vol);
    return rc;
}
