// SPDX-License-Identifier: GPL-3.0-or-later
static int truncate_file_tx(struct infs_volume *vol, const char *path, uint64_t size)
{
    if (!vol->writable)
        return INFS_STATUS_READ_ONLY;

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
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    struct infs_file_payload_disk *p;
    struct infs_extent_disk *ext;
    status = file_validate_volume(vol, object, &p, &ext);
    if (status != INFS_STATUS_OK)
        return status;

    uint64_t old_size = infs_le64_to_cpu(p->attributes.logical_size);
    int inline_enabled =
        (infs_le64_to_cpu(vol->sb.incompat_flags) &
         INFS_INCOMPAT_INLINE_DATA) != 0;

    if (inline_enabled && size <= INFS_INLINE_DATA_MAX) {
        uint8_t inline_data[INFS_INLINE_DATA_MAX] = {0};
        uint64_t preserve64 = old_size < size ? old_size : size;
        if (preserve64) {
            int64_t got = infs_read_file(vol, path, inline_data,
                                         (size_t)preserve64, 0);
            if (got < 0)
                return (infs_status)got;
            if ((uint64_t)got != preserve64)
                return INFS_STATUS_CORRUPT;
        }
        status = file_store_inline(vol, object, inline_data, (size_t)size);
        if (status != INFS_STATUS_OK)
            return status;
        p = (struct infs_file_payload_disk *)(
            object + sizeof(struct infs_object_header_disk));
        touch_mctime(vol, &p->attributes);
        if (vol->tx_error != INFS_STATUS_OK)
            return vol->tx_error;
        status = infs_object_finalize(object);
        if (status != INFS_STATUS_OK)
            return status;
        status = file_validate_volume(vol, object, &p, &ext);
        if (status != INFS_STATUS_OK)
            return status;
        status = write_object(vol, lk.block, object);
        if (status != INFS_STATUS_OK)
            return status;
        return finish_mutation(vol, (uint64_t)INFS_BLOCK_SIZE * 2u);
    }

    if (file_is_inline(object, p)) {
        status = file_promote_inline(vol, object);
        if (status != INFS_STATUS_OK)
            return status;
        status = file_validate_volume(vol, object, &p, &ext);
        if (status != INFS_STATUS_OK)
            return status;
    }

    uint64_t target_blocks = size / INFS_BLOCK_SIZE +
        ((size % INFS_BLOCK_SIZE) != 0);
    uint64_t have = file_logical_blocks(p, ext);
    if (target_blocks > have) {
        status = file_ensure_logical_blocks(vol, object, target_blocks);
        if (status != INFS_STATUS_OK)
            return status;
    } else if (target_blocks < have) {
        uint32_t count = infs_le32_to_cpu(p->extent_count);
        uint32_t keep = 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t logical = infs_le64_to_cpu(ext[i].logical_block);
            uint64_t physical = infs_le64_to_cpu(ext[i].physical_block);
            uint32_t blocks = infs_le32_to_cpu(ext[i].block_count);
            uint32_t flags = infs_le32_to_cpu(ext[i].flags);
            if (logical >= target_blocks) {
                if (flags == INFS_EXTENT_NORMAL) {
                    status = file_free_unshared_run(
                        vol, hdr->object_id, physical, blocks);
                    if (status != INFS_STATUS_OK)
                        return status;
                }
                continue;
            }
            uint64_t extent_end = logical + blocks;
            if (extent_end > target_blocks) {
                uint32_t retained = (uint32_t)(target_blocks - logical);
                if (flags == INFS_EXTENT_NORMAL) {
                    status = file_free_unshared_run(
                        vol, hdr->object_id, physical + retained,
                        blocks - retained);
                    if (status != INFS_STATUS_OK)
                        return status;
                }
                ext[i].block_count = infs_cpu_to_le32(retained);
            }
            if (keep != i)
                ext[keep] = ext[i];
            ++keep;
        }
        if (keep < count)
            memset(&ext[keep], 0, (size_t)(count - keep) * sizeof(*ext));
        p->extent_count = infs_cpu_to_le32(keep);
        hdr->payload_size = infs_cpu_to_le32((uint32_t)(sizeof(*p) +
            (size_t)keep * sizeof(*ext)));
    }

    p->attributes.logical_size = infs_cpu_to_le64(size);
    status = infs_object_finalize(object);
    if (status != INFS_STATUS_OK)
        return status;
    status = file_validate_volume(vol, object, &p, &ext);
    if (status != INFS_STATUS_OK)
        return status;

    if (size < old_size && target_blocks > 0 &&
        (size % INFS_BLOCK_SIZE) != 0) {
        uint64_t logical = target_blocks - 1u;
        uint64_t physical;
        uint32_t flags;
        status = file_map_block(p, ext, logical, &physical, &flags);
        if (status != INFS_STATUS_OK)
            return status;
        if (flags == INFS_EXTENT_NORMAL) {
            uint8_t data[INFS_BLOCK_SIZE];
            status = read_block(vol, physical, data);
            if (status != INFS_STATUS_OK)
                return status;
            status = data_block_verify(vol, p, hdr->object_id, logical, data);
            if (status != INFS_STATUS_OK)
                return status;
            size_t tail = (size_t)(size % INFS_BLOCK_SIZE);
            memset(data + tail, 0, INFS_BLOCK_SIZE - tail);
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
            struct infs_data_checksum_disk digest;
            data_block_checksum(data, &digest);
            status = checksum_set_range(vol, p, hdr->object_id, logical,
                                        &digest, 1);
            if (status != INFS_STATUS_OK)
                return status;
        }
    }

    if (file_allocated_blocks(p, ext) == 0 &&
        !id_is_zero(p->checksum_head_id)) {
        status = checksum_destroy_chain(vol, p, hdr->object_id);
        if (status != INFS_STATUS_OK)
            return status;
    }
    touch_mctime(vol, &p->attributes);
    if (vol->tx_error != INFS_STATUS_OK)
        return vol->tx_error;
    status = write_object(vol, lk.block, object);
    if (status != INFS_STATUS_OK)
        return status;
    return finish_mutation(vol, (uint64_t)INFS_BLOCK_SIZE * 4u);
}

infs_status infs_truncate_file(struct infs_volume *vol, const char *path,
                               uint64_t size)
{
    if (!vol || !path)
        return INFS_STATUS_INVALID_ARGUMENT;
    infs_status status = tx_begin(vol);
    if (status != INFS_STATUS_OK)
        return status;
    status = truncate_file_tx(vol, path, size);
    if (status != INFS_STATUS_OK)
        tx_abort(vol);
    return status;
}
