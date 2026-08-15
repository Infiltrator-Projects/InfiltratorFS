// SPDX-License-Identifier: GPL-3.0-or-later
static int truncate_file_tx(struct infs_volume *vol, const char *path, uint64_t size)
{
    if (!vol->writable) {
        return INFS_STATUS_READ_ONLY;
    }
    struct infs_lookup lk;
    if (infs_lookup_path(vol, path, &lk) != 0)
        return INFS_STATUS_ERROR;
    if (lk.type != INFS_OBJECT_FILE) {
        return INFS_STATUS_IS_DIRECTORY;
    }
    uint8_t object[INFS_BLOCK_SIZE];
    if (read_object(vol, lk.block, object) != 0)
        return INFS_STATUS_ERROR;
    struct infs_file_payload_disk *p;
    struct infs_extent_disk *ext;
    if (file_validate_volume(vol, object, &p, &ext) != 0)
        return INFS_STATUS_ERROR;

    uint64_t old_size = infs_le64_to_cpu(p->attributes.logical_size);
    if (size > UINT64_MAX - (INFS_BLOCK_SIZE - 1u)) {
        return INFS_STATUS_FILE_TOO_LARGE;
    }
    uint64_t target_blocks = (size + INFS_BLOCK_SIZE - 1u) / INFS_BLOCK_SIZE;
    uint64_t have = file_allocated_blocks(p, ext);
    if (target_blocks > have) {
        if (file_ensure_blocks(vol, object, target_blocks) != 0)
            return INFS_STATUS_ERROR;
        if (infs_object_finalize(object) != 0 ||
            file_validate_volume(vol, object, &p, &ext) != 0)
            return INFS_STATUS_ERROR;
    } else if (target_blocks < have) {
        uint32_t count = infs_le32_to_cpu(p->extent_count);
        uint32_t keep = 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t logical = infs_le64_to_cpu(ext[i].logical_block);
            uint64_t physical = infs_le64_to_cpu(ext[i].physical_block);
            uint32_t blocks = infs_le32_to_cpu(ext[i].block_count);
            if (logical >= target_blocks) {
                free_run(vol, physical, blocks);
                continue;
            }
            uint64_t end = logical + blocks;
            if (end > target_blocks) {
                uint32_t retained = (uint32_t)(target_blocks - logical);
                free_run(vol, physical + retained, blocks - retained);
                ext[i].block_count = infs_cpu_to_le32(retained);
            }
            if (keep != i)
                ext[keep] = ext[i];
            ++keep;
        }
        if (keep < count)
            memset(&ext[keep], 0, (size_t)(count - keep) * sizeof(*ext));
        p->extent_count = infs_cpu_to_le32(keep);
        struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)object;
        hdr->payload_size = infs_cpu_to_le32((uint32_t)(sizeof(*p) +
            (size_t)keep * sizeof(*ext)));
    }

    if (size < old_size && target_blocks > 0 && (size % INFS_BLOCK_SIZE) != 0) {
        if (infs_object_finalize(object) != 0 ||
            file_validate_volume(vol, object, &p, &ext) != 0)
            return INFS_STATUS_ERROR;
        struct infs_object_header_disk *file_hdr =
            (struct infs_object_header_disk *)object;
        uint64_t logical = target_blocks - 1u;
        uint64_t physical;
        if (file_map_block(p, ext, logical, &physical) != 0)
            return INFS_STATUS_ERROR;
        uint8_t data[INFS_BLOCK_SIZE];
        if (read_block(vol, physical, data) != 0 ||
            data_block_verify(vol, p, file_hdr->object_id, logical, data) != 0)
            return INFS_STATUS_ERROR;
        size_t tail = (size_t)(size % INFS_BLOCK_SIZE);
        memset(data + tail, 0, INFS_BLOCK_SIZE - tail);
        uint64_t replacement = 0, got = 0;
        int status = alloc_run(vol, 1, &replacement, &got);
        if (status != INFS_STATUS_OK)
            return status;
        if (got != 1)
            return INFS_STATUS_CORRUPT;
        if (write_block(vol, replacement, data) != 0 ||
            file_replace_range(vol, object, logical, 1, replacement) != 0)
            return INFS_STATUS_ERROR;
        struct infs_data_checksum_disk digest;
        data_block_checksum(data, &digest);
        if (checksum_set_range(vol, p, file_hdr->object_id, logical, &digest, 1) != 0)
            return INFS_STATUS_ERROR;
    }

    p->attributes.logical_size = infs_cpu_to_le64(size);
    touch_mctime(vol, &p->attributes);
    if (write_object(vol, lk.block, object) != 0)
        return INFS_STATUS_ERROR;
    return infs_volume_sync(vol);
}

infs_status infs_truncate_file(struct infs_volume *vol, const char *path,
                               uint64_t size)
{
    infs_status status = tx_begin(vol);
    if (status != INFS_STATUS_OK)
        return status;
    status = truncate_file_tx(vol, path, size);
    if (status != INFS_STATUS_OK)
        tx_abort(vol);
    return status;
}
