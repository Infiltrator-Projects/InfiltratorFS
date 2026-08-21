// SPDX-License-Identifier: GPL-3.0-or-later
static infs_status file_other_reference_cover(
    struct infs_volume *vol, const uint8_t owner_id[16],
    uint64_t cursor, uint64_t end, uint64_t *cover_end,
    uint64_t *next_start)
{
    *cover_end = cursor;
    *next_start = end;

    struct infs_index_entry_disk *entries = NULL;
    uint32_t count = 0;
    infs_status status = index_snapshot(vol, &entries, &count);
    if (status != INFS_STATUS_OK)
        return status;

    for (uint32_t i = 0; i < count; ++i) {
        if (infs_le16_to_cpu(entries[i].object_type) != INFS_OBJECT_FILE ||
            memcmp(entries[i].object_id, owner_id, 16) == 0)
            continue;

        uint8_t object[INFS_BLOCK_SIZE];
        status = read_object(vol, infs_le64_to_cpu(entries[i].object_block),
                             object);
        if (status != INFS_STATUS_OK) {
            free(entries);
            return status;
        }
        struct infs_file_payload_disk *file;
        struct infs_extent_disk *ext;
        status = file_validate_volume(vol, object, &file, &ext);
        if (status != INFS_STATUS_OK) {
            free(entries);
            return status;
        }

        uint32_t extent_count = infs_le32_to_cpu(file->extent_count);
        for (uint32_t j = 0; j < extent_count; ++j) {
            if (infs_le32_to_cpu(ext[j].flags) != INFS_EXTENT_NORMAL)
                continue;
            uint64_t physical = infs_le64_to_cpu(ext[j].physical_block);
            uint64_t blocks = infs_le32_to_cpu(ext[j].block_count);
            uint64_t extent_end = physical + blocks;
            if (extent_end <= cursor || physical >= end)
                continue;
            if (physical <= cursor) {
                uint64_t candidate = extent_end < end ? extent_end : end;
                if (candidate > *cover_end)
                    *cover_end = candidate;
            } else if (physical < *next_start) {
                *next_start = physical;
            }
        }
    }
    free(entries);
    return INFS_STATUS_OK;
}

static infs_status file_free_unshared_run(
    struct infs_volume *vol, const uint8_t owner_id[16],
    uint64_t start, uint64_t count)
{
    if (!count)
        return INFS_STATUS_OK;
    if ((infs_le64_to_cpu(vol->sb.incompat_flags) &
         INFS_INCOMPAT_SHARED_EXTENTS) == 0) {
        free_run(vol, start, count);
        return vol->tx_error;
    }
    if (start > UINT64_MAX - count)
        return INFS_STATUS_OVERFLOW;

    uint64_t cursor = start;
    uint64_t end = start + count;
    while (cursor < end) {
        uint64_t cover_end, next_start;
        infs_status status = file_other_reference_cover(
            vol, owner_id, cursor, end, &cover_end, &next_start);
        if (status != INFS_STATUS_OK)
            return status;
        if (cover_end > cursor) {
            cursor = cover_end;
            continue;
        }
        if (next_start <= cursor || next_start > end)
            return INFS_STATUS_CORRUPT;
        free_run(vol, cursor, next_start - cursor);
        if (vol->tx_error != INFS_STATUS_OK)
            return vol->tx_error;
        cursor = next_start;
    }
    return INFS_STATUS_OK;
}

static infs_status reflink_copy_checksums(
    struct infs_volume *vol,
    struct infs_file_payload_disk *source,
    const uint8_t source_id[16],
    struct infs_file_payload_disk *destination,
    const uint8_t destination_id[16],
    const struct infs_extent_disk *extents, uint32_t extent_count)
{
    struct infs_data_checksum_disk digests[INFS_CHECKSUMS_PER_OBJECT];
    for (uint32_t i = 0; i < extent_count; ++i) {
        if (infs_le32_to_cpu(extents[i].flags) != INFS_EXTENT_NORMAL)
            continue;
        uint64_t logical = infs_le64_to_cpu(extents[i].logical_block);
        uint64_t remaining = infs_le32_to_cpu(extents[i].block_count);
        while (remaining) {
            uint32_t chunk = remaining > INFS_CHECKSUMS_PER_OBJECT ?
                INFS_CHECKSUMS_PER_OBJECT : (uint32_t)remaining;
            for (uint32_t j = 0; j < chunk; ++j) {
                infs_status status = checksum_get(
                    vol, source, source_id, logical + j, &digests[j]);
                if (status != INFS_STATUS_OK)
                    return status;
            }
            infs_status status = checksum_set_range(
                vol, destination, destination_id, logical, digests, chunk);
            if (status != INFS_STATUS_OK)
                return status;
            logical += chunk;
            remaining -= chunk;
        }
    }
    return INFS_STATUS_OK;
}

static infs_status reflink_file_tx(struct infs_volume *vol,
                                   const char *source_path,
                                   const char *destination_path)
{
    if (!vol->writable)
        return INFS_STATUS_READ_ONLY;
    if ((infs_le64_to_cpu(vol->sb.incompat_flags) &
         INFS_INCOMPAT_SHARED_EXTENTS) == 0)
        return INFS_STATUS_NOT_SUPPORTED;

    struct infs_lookup source_lookup;
    infs_status status = infs_lookup_path(vol, source_path, &source_lookup);
    if (status != INFS_STATUS_OK)
        return status;
    if (source_lookup.type != INFS_OBJECT_FILE)
        return INFS_STATUS_IS_DIRECTORY;

    uint8_t source_object[INFS_BLOCK_SIZE];
    status = read_object(vol, source_lookup.block, source_object);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_file_payload_disk *source;
    struct infs_extent_disk *source_extents;
    status = file_validate_volume(vol, source_object, &source, &source_extents);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_create_options options = {0};
    options.portable_flags = infs_le64_to_cpu(source->attributes.portable_flags);
    options.posix_permissions = infs_le32_to_cpu(source->posix.permissions);
    options.posix_uid = infs_le32_to_cpu(source->posix.uid);
    options.posix_gid = infs_le32_to_cpu(source->posix.gid);
    status = create_object(vol, destination_path, &options, INFS_OBJECT_FILE);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_lookup destination_lookup;
    status = infs_lookup_path(vol, destination_path, &destination_lookup);
    if (status != INFS_STATUS_OK)
        return status;
    uint8_t destination_object[INFS_BLOCK_SIZE];
    status = read_object(vol, destination_lookup.block, destination_object);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_file_payload_disk *destination;
    struct infs_extent_disk *destination_extents;
    status = file_validate_volume(vol, destination_object, &destination,
                                  &destination_extents);
    if (status != INFS_STATUS_OK)
        return status;

    uint64_t logical_size = infs_le64_to_cpu(source->attributes.logical_size);
    if (file_is_inline(source_object, source)) {
        uint8_t data[INFS_INLINE_DATA_MAX];
        status = file_read_small_content(vol, source_object, data);
        if (status != INFS_STATUS_OK)
            return status;
        status = file_store_inline(vol, destination_object, data,
                                   (size_t)logical_size);
        if (status != INFS_STATUS_OK)
            return status;
    } else {
        uint32_t extent_count = infs_le32_to_cpu(source->extent_count);
        memset(destination + 1, 0,
               INFS_BLOCK_SIZE - sizeof(struct infs_object_header_disk) -
               sizeof(*destination));
        destination->attributes.logical_size = infs_cpu_to_le64(logical_size);
        destination->extent_count = infs_cpu_to_le32(extent_count);
        destination->data_checksum_type = infs_cpu_to_le32(INFS_CHECKSUM_SHA256);
        memset(destination->checksum_head_id, 0,
               sizeof(destination->checksum_head_id));
        destination_extents = (struct infs_extent_disk *)(destination + 1);
        memcpy(destination_extents, source_extents,
               (size_t)extent_count * sizeof(*destination_extents));
        struct infs_object_header_disk *destination_header =
            (struct infs_object_header_disk *)destination_object;
        destination_header->payload_size = infs_cpu_to_le32((uint32_t)(
            sizeof(*destination) +
            (size_t)extent_count * sizeof(*destination_extents)));
        struct infs_object_header_disk *source_header =
            (struct infs_object_header_disk *)source_object;
        status = reflink_copy_checksums(
            vol, source, source_header->object_id, destination,
            destination_header->object_id, destination_extents, extent_count);
        if (status != INFS_STATUS_OK)
            return status;
    }

    status = infs_object_finalize(destination_object);
    if (status != INFS_STATUS_OK)
        return status;
    status = write_object(vol, destination_lookup.block, destination_object);
    if (status != INFS_STATUS_OK)
        return status;
    return finish_mutation(vol, logical_size + INFS_BLOCK_SIZE);
}

infs_status infs_reflink_file(struct infs_volume *vol, const char *source_path,
                              const char *destination_path)
{
    if (!vol || !source_path || !destination_path ||
        source_path[0] != '/' || destination_path[0] != '/')
        return INFS_STATUS_INVALID_ARGUMENT;
    size_t destination_length = strlen(destination_path);
    if (destination_length > 1u &&
        destination_path[destination_length - 1u] == '/')
        return INFS_STATUS_NOT_DIRECTORY;

    infs_status status = tx_begin(vol);
    if (status != INFS_STATUS_OK)
        return status;
    status = reflink_file_tx(vol, source_path, destination_path);
    if (status != INFS_STATUS_OK)
        tx_abort(vol);
    return status;
}
