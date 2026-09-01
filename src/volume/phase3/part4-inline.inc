// SPDX-License-Identifier: GPL-3.0-or-later
static infs_status file_read_small_content(
    struct infs_volume *vol, uint8_t object[INFS_BLOCK_SIZE],
    uint8_t data[INFS_INLINE_DATA_MAX])
{
    struct infs_file_payload_disk *file;
    struct infs_extent_disk *ext;
    infs_status status = file_validate_volume(vol, object, &file, &ext);
    if (status != INFS_STATUS_OK)
        return status;

    uint64_t size = infs_le64_to_cpu(file->attributes.logical_size);
    if (size > INFS_INLINE_DATA_MAX)
        return INFS_STATUS_FILE_TOO_LARGE;
    memset(data, 0, INFS_INLINE_DATA_MAX);
    if (size == 0)
        return INFS_STATUS_OK;
    if (file_is_inline(object, file)) {
        memcpy(data, file_inline_bytes(file), (size_t)size);
        return INFS_STATUS_OK;
    }

    uint64_t physical;
    uint32_t flags;
    status = file_map_block(vol, object, file, ext, 0, &physical, &flags);
    if (status != INFS_STATUS_OK)
        return status;
    if (extent_kind(flags) == INFS_EXTENT_HOLE)
        return INFS_STATUS_OK;

    uint8_t block[INFS_BLOCK_SIZE];
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    status = file_read_logical_block(
        vol, object, file, ext, hdr->object_id, 0, block);
    if (status != INFS_STATUS_OK)
        return status;
    memcpy(data, block, (size_t)size);
    return INFS_STATUS_OK;
}

static infs_status file_release_external_storage(
    struct infs_volume *vol, uint8_t object[INFS_BLOCK_SIZE])
{
    struct infs_file_payload_disk *file;
    struct infs_extent_disk *ext;
    infs_status status = file_validate_volume(vol, object, &file, &ext);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    if (!id_is_zero(file->checksum_head_id)) {
        status = checksum_destroy_chain(vol, file, hdr->object_id);
        if (status != INFS_STATUS_OK)
            return status;
    }

    struct infs_extent_disk *all = NULL;
    uint32_t count = 0;
    int owned = 0;
    status = file_extent_snapshot(vol, object, file, ext, &all, &count, &owned);
    if (status != INFS_STATUS_OK)
        return status;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t flags = infs_le32_to_cpu(all[i].flags);
        uint32_t logical_blocks = infs_le32_to_cpu(all[i].block_count);
        if (extent_kind(flags) != INFS_EXTENT_NORMAL)
            continue;
        status = file_free_unshared_run(
            vol, hdr->object_id, infs_le64_to_cpu(all[i].physical_block),
            extent_physical_blocks(logical_blocks, flags));
        if (status != INFS_STATUS_OK) {
            if (owned) free(all);
            return status;
        }
    }
    if (owned) free(all);
    return paged_extent_destroy_pages(vol, object, file);
}

static infs_status file_store_inline(
    struct infs_volume *vol, uint8_t object[INFS_BLOCK_SIZE],
    const uint8_t *data, size_t size)
{
    if (size > INFS_INLINE_DATA_MAX || (size && !data))
        return INFS_STATUS_INVALID_ARGUMENT;

    struct infs_file_payload_disk *file;
    struct infs_extent_disk *ext;
    infs_status status = file_validate_volume(vol, object, &file, &ext);
    if (status != INFS_STATUS_OK)
        return status;
    (void)ext;

    status = file_release_external_storage(vol, object);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    file = (struct infs_file_payload_disk *)(object + sizeof(*hdr));
    hdr->object_version = infs_cpu_to_le16(INFS_OBJECT_VERSION_CLASSIC);
    memset(file + 1, 0,
           INFS_BLOCK_SIZE - sizeof(*hdr) - sizeof(*file));
    file->attributes.logical_size = infs_cpu_to_le64((uint64_t)size);
    file->extent_count = 0;
    file->data_checksum_type = infs_cpu_to_le32(INFS_CHECKSUM_SHA256);
    memset(file->checksum_head_id, 0, sizeof(file->checksum_head_id));

    if (size == 0) {
        hdr->payload_size = infs_cpu_to_le32(sizeof(*file));
        return INFS_STATUS_OK;
    }

    struct infs_data_checksum_disk *digest = file_inline_digest(file);
    uint8_t *bytes = file_inline_bytes(file);
    memcpy(bytes, data, size);
    file_inline_checksum(bytes, size, digest);
    hdr->payload_size = infs_cpu_to_le32((uint32_t)(
        sizeof(*file) + sizeof(*digest) + size));
    return INFS_STATUS_OK;
}

static infs_status file_promote_inline(
    struct infs_volume *vol, uint8_t object[INFS_BLOCK_SIZE])
{
    struct infs_file_payload_disk *file;
    struct infs_extent_disk *ext;
    infs_status status = file_validate_volume(vol, object, &file, &ext);
    if (status != INFS_STATUS_OK)
        return status;
    if (!file_is_inline(object, file))
        return INFS_STATUS_OK;

    uint64_t size = infs_le64_to_cpu(file->attributes.logical_size);
    uint8_t data[INFS_BLOCK_SIZE] = {0};
    memcpy(data, file_inline_bytes(file), (size_t)size);

    uint64_t physical = 0, got = 0;
    status = alloc_data_run(vol, 1, &physical, &got);
    if (status != INFS_STATUS_OK)
        return status;
    if (got != 1)
        return INFS_STATUS_CORRUPT;
    status = write_block(vol, physical, data);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    file = (struct infs_file_payload_disk *)(object + sizeof(*hdr));
    memset(file + 1, 0,
           INFS_BLOCK_SIZE - sizeof(*hdr) - sizeof(*file));
    file->extent_count = infs_cpu_to_le32(1);
    memset(file->checksum_head_id, 0, sizeof(file->checksum_head_id));
    ext = (struct infs_extent_disk *)(file + 1);
    ext[0].logical_block = 0;
    ext[0].physical_block = infs_cpu_to_le64(physical);
    ext[0].block_count = infs_cpu_to_le32(1);
    ext[0].flags = infs_cpu_to_le32(INFS_EXTENT_NORMAL);
    hdr->payload_size = infs_cpu_to_le32(
        sizeof(*file) + sizeof(struct infs_extent_disk));

    struct infs_data_checksum_disk digest;
    data_block_checksum(data, &digest);
    status = checksum_set_range(vol, file, hdr->object_id, 0, &digest, 1);
    if (status != INFS_STATUS_OK)
        return status;

    status = infs_object_finalize(object);
    if (status != INFS_STATUS_OK)
        return status;
    return file_validate_volume(vol, object, NULL, NULL);
}
