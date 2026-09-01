// SPDX-License-Identifier: GPL-3.0-or-later

static int file_extent_lookup(struct infs_volume *vol,
                              uint8_t object[INFS_BLOCK_SIZE],
                              struct infs_file_payload_disk *file,
                              struct infs_extent_disk *classic,
                              uint64_t logical,
                              struct infs_extent_disk *out)
{
    if (!out)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (file_has_paged_extents(object)) {
        uint8_t page[INFS_BLOCK_SIZE];
        struct infs_extent_disk *extents = NULL;
        uint32_t count = 0;
        infs_status status = paged_extent_find_page(
            vol, object, file, logical, NULL, page, &extents, &count);
        if (status != INFS_STATUS_OK)
            return status;
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t start = infs_le64_to_cpu(extents[i].logical_block);
            uint64_t end = start + infs_le32_to_cpu(extents[i].block_count);
            if (logical >= start && logical < end) {
                *out = extents[i];
                return INFS_STATUS_OK;
            }
        }
        return INFS_STATUS_CORRUPT;
    }
    uint32_t count = infs_le32_to_cpu(file->extent_count);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t start = infs_le64_to_cpu(classic[i].logical_block);
        uint64_t end = start + infs_le32_to_cpu(classic[i].block_count);
        if (logical >= start && logical < end) {
            *out = classic[i];
            return INFS_STATUS_OK;
        }
    }
    return INFS_STATUS_CORRUPT;
}

static int file_decompress_extent(struct infs_volume *vol,
                                  const struct infs_extent_disk *extent,
                                  uint8_t **data_out, size_t *bytes_out)
{
    uint32_t logical_blocks = infs_le32_to_cpu(extent->block_count);
    uint32_t flags = infs_le32_to_cpu(extent->flags);
    uint32_t stored = extent_stored_bytes(flags);
    uint64_t physical = infs_le64_to_cpu(extent->physical_block);
    uint64_t physical_blocks = extent_physical_blocks(logical_blocks, flags);
    size_t logical_bytes = (size_t)logical_blocks * INFS_BLOCK_SIZE;
    size_t stored_span = (size_t)physical_blocks * INFS_BLOCK_SIZE;
    uint8_t *compressed = NULL;
    uint8_t *plain = NULL;
    int decoded;
    infs_status status;

    if (!extent_is_compressed(flags) || !stored || !physical_blocks ||
        logical_blocks > INFS_COMPRESSION_CLUSTER_BLOCKS ||
        stored > stored_span)
        return INFS_STATUS_CORRUPT;
    compressed = malloc(stored_span);
    plain = malloc(logical_bytes);
    if (!compressed || !plain) {
        free(compressed);
        free(plain);
        return INFS_STATUS_NO_MEMORY;
    }
    status = infs_storage_read(&vol->storage, physical * INFS_BLOCK_SIZE,
                               compressed, stored_span);
    if (status != INFS_STATUS_OK) {
        free(compressed);
        free(plain);
        return status;
    }
    decoded = LZ4_decompress_safe((const char *)compressed, (char *)plain,
                                  (int)stored, (int)logical_bytes);
    free(compressed);
    if (decoded != (int)logical_bytes) {
        free(plain);
        return INFS_STATUS_CORRUPT;
    }
    *data_out = plain;
    if (bytes_out)
        *bytes_out = logical_bytes;
    return INFS_STATUS_OK;
}

static int file_read_logical_block(struct infs_volume *vol,
                                   uint8_t object[INFS_BLOCK_SIZE],
                                   struct infs_file_payload_disk *file,
                                   struct infs_extent_disk *classic,
                                   const uint8_t owner_id[16],
                                   uint64_t logical,
                                   uint8_t data[INFS_BLOCK_SIZE])
{
    struct infs_extent_disk extent;
    infs_status status = file_extent_lookup(
        vol, object, file, classic, logical, &extent);
    if (status != INFS_STATUS_OK)
        return status;
    uint64_t start = infs_le64_to_cpu(extent.logical_block);
    uint64_t physical = infs_le64_to_cpu(extent.physical_block);
    uint32_t flags = infs_le32_to_cpu(extent.flags);

    if (extent_kind(flags) == INFS_EXTENT_HOLE) {
        memset(data, 0, INFS_BLOCK_SIZE);
        return INFS_STATUS_OK;
    }
    if (extent_is_compressed(flags)) {
        uint8_t *plain = NULL;
        size_t bytes = 0;
        status = file_decompress_extent(vol, &extent, &plain, &bytes);
        if (status != INFS_STATUS_OK)
            return status;
        uint64_t delta = logical - start;
        if (delta >= infs_le32_to_cpu(extent.block_count) ||
            (delta + 1u) * INFS_BLOCK_SIZE > bytes) {
            free(plain);
            return INFS_STATUS_CORRUPT;
        }
        memcpy(data, plain + (size_t)delta * INFS_BLOCK_SIZE, INFS_BLOCK_SIZE);
        free(plain);
    } else {
        status = read_block(vol, physical + (logical - start), data);
        if (status != INFS_STATUS_OK)
            return status;
    }
    return data_block_verify(vol, file, owner_id, logical, data);
}

static int file_materialize_compressed_extent(
    struct infs_volume *vol, uint8_t object[INFS_BLOCK_SIZE],
    struct infs_file_payload_disk *file, struct infs_extent_disk *classic,
    const struct infs_extent_disk *extent)
{
    uint32_t flags = infs_le32_to_cpu(extent->flags);
    uint32_t blocks = infs_le32_to_cpu(extent->block_count);
    uint64_t logical = infs_le64_to_cpu(extent->logical_block);
    uint64_t replacement = 0;
    uint8_t *plain = NULL;
    size_t plain_bytes = 0;
    infs_status status;

    (void)classic;
    if (!extent_is_compressed(flags))
        return INFS_STATUS_OK;
    status = file_decompress_extent(vol, extent, &plain, &plain_bytes);
    if (status != INFS_STATUS_OK)
        return status;
    status = alloc_exact_run_uninitialized(vol, blocks, &replacement);
    if (status != INFS_STATUS_OK) {
        free(plain);
        return status;
    }
    status = infs_storage_write(&vol->storage, replacement * INFS_BLOCK_SIZE,
                                plain, plain_bytes);
    free(plain);
    if (status != INFS_STATUS_OK)
        return status;
    status = file_replace_range(vol, object, logical, blocks,
                                replacement, INFS_EXTENT_NORMAL);
    if (status != INFS_STATUS_OK)
        return status;
    return file_validate_volume(vol, object, &file, &classic);
}

static int file_materialize_compressed_at(
    struct infs_volume *vol, uint8_t object[INFS_BLOCK_SIZE],
    struct infs_file_payload_disk **file_io,
    struct infs_extent_disk **classic_io, uint64_t logical)
{
    struct infs_extent_disk extent;
    infs_status status = file_extent_lookup(
        vol, object, *file_io, *classic_io, logical, &extent);
    if (status != INFS_STATUS_OK)
        return status;
    if (!extent_is_compressed(infs_le32_to_cpu(extent.flags)))
        return INFS_STATUS_OK;
    status = file_materialize_compressed_extent(
        vol, object, *file_io, *classic_io, &extent);
    if (status != INFS_STATUS_OK)
        return status;
    return file_validate_volume(vol, object, file_io, classic_io);
}

static int file_materialize_compressed_range(
    struct infs_volume *vol, uint8_t object[INFS_BLOCK_SIZE],
    struct infs_file_payload_disk **file_io,
    struct infs_extent_disk **classic_io,
    uint64_t logical_start, uint64_t logical_count)
{
    uint64_t end = logical_start + logical_count;
    uint64_t cursor = logical_start;
    if (end < logical_start)
        return INFS_STATUS_OVERFLOW;
    while (cursor < end) {
        struct infs_extent_disk extent;
        infs_status status = file_extent_lookup(
            vol, object, *file_io, *classic_io, cursor, &extent);
        if (status != INFS_STATUS_OK)
            return status;
        uint64_t start = infs_le64_to_cpu(extent.logical_block);
        uint64_t extent_end = start + infs_le32_to_cpu(extent.block_count);
        if (extent_is_compressed(infs_le32_to_cpu(extent.flags))) {
            status = file_materialize_compressed_extent(
                vol, object, *file_io, *classic_io, &extent);
            if (status != INFS_STATUS_OK)
                return status;
            status = file_validate_volume(vol, object, file_io, classic_io);
            if (status != INFS_STATUS_OK)
                return status;
        }
        if (extent_end <= cursor)
            return INFS_STATUS_CORRUPT;
        cursor = extent_end < end ? extent_end : end;
    }
    return INFS_STATUS_OK;
}

static int file_store_extent_payload(
    struct infs_volume *vol, const uint8_t *plain, uint32_t logical_blocks,
    uint64_t *physical_out, uint32_t *flags_out)
{
    size_t plain_bytes = (size_t)logical_blocks * INFS_BLOCK_SIZE;
    int bound;
    uint8_t *compressed = NULL;
    uint8_t *stored = NULL;
    int compressed_bytes = 0;
    uint32_t stored_blocks;
    uint64_t physical = 0;
    infs_status status;

    if (!logical_blocks || logical_blocks > INFS_COMPRESSION_CLUSTER_BLOCKS)
        return INFS_STATUS_INVALID_ARGUMENT;
    if ((infs_le64_to_cpu(vol->sb.incompat_flags) &
         INFS_INCOMPAT_COMPRESSED_EXTENTS) != 0 && logical_blocks >= 2u) {
        bound = LZ4_compressBound((int)plain_bytes);
        compressed = malloc((size_t)bound);
        if (!compressed)
            return INFS_STATUS_NO_MEMORY;
        compressed_bytes = LZ4_compress_default(
            (const char *)plain, (char *)compressed,
            (int)plain_bytes, bound);
    }
    if (compressed_bytes > 0 &&
        (uint32_t)compressed_bytes <= INFS_EXTENT_STORED_BYTES_MAX) {
        stored_blocks = (uint32_t)compressed_bytes / INFS_BLOCK_SIZE +
            (((uint32_t)compressed_bytes % INFS_BLOCK_SIZE) != 0);
        if (stored_blocks < logical_blocks) {
            size_t span = (size_t)stored_blocks * INFS_BLOCK_SIZE;
            stored = calloc(1u, span);
            if (!stored) {
                free(compressed);
                return INFS_STATUS_NO_MEMORY;
            }
            memcpy(stored, compressed, (size_t)compressed_bytes);
            status = alloc_exact_run_uninitialized(vol, stored_blocks,
                                                   &physical);
            if (status == INFS_STATUS_OK)
                status = infs_storage_write(
                    &vol->storage, physical * INFS_BLOCK_SIZE, stored, span);
            free(stored);
            free(compressed);
            if (status != INFS_STATUS_OK)
                return status;
            *physical_out = physical;
            *flags_out = extent_compressed_flags(
                INFS_COMPRESSION_LZ4, (uint32_t)compressed_bytes);
            return INFS_STATUS_OK;
        }
    }
    free(compressed);
    status = alloc_exact_run_uninitialized(vol, logical_blocks, &physical);
    if (status != INFS_STATUS_OK)
        return status;
    status = infs_storage_write(&vol->storage, physical * INFS_BLOCK_SIZE,
                                plain, plain_bytes);
    if (status != INFS_STATUS_OK)
        return status;
    *physical_out = physical;
    *flags_out = INFS_EXTENT_NORMAL;
    return INFS_STATUS_OK;
}
