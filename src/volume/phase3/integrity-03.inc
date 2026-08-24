// SPDX-License-Identifier: GPL-3.0-or-later
static int checksum_set_range(struct infs_volume *vol,
                              struct infs_file_payload_disk *file,
                              const uint8_t owner_id[16], uint64_t logical,
                              const struct infs_data_checksum_disk *digests,
                              uint64_t count)
{
    if (!vol || !file || !owner_id || (count && !digests))
        return INFS_STATUS_INVALID_ARGUMENT;

    while (count) {
        uint64_t offset = logical % INFS_CHECKSUMS_PER_OBJECT;
        uint64_t chunk = INFS_CHECKSUMS_PER_OBJECT - offset;
        if (chunk > count)
            chunk = count;

        struct infs_lookup lk;
        uint8_t block[INFS_BLOCK_SIZE];
        struct infs_checksum_payload_disk *p;
        struct infs_data_checksum_disk *values;
        infs_status status = checksum_locate(vol, file, owner_id, logical, 1,
                                             &lk, block, &p, &values);
        if (status != INFS_STATUS_OK)
            return status;
        for (uint64_t i = 0; i < chunk; ++i)
            values[offset + i] = digests[i];
        uint32_t needed = (uint32_t)(offset + chunk);
        if (needed > infs_le32_to_cpu(p->checksum_count))
            p->checksum_count = infs_cpu_to_le32(needed);
        struct infs_object_header_disk *hdr =
            (struct infs_object_header_disk *)block;
        hdr->payload_size = infs_cpu_to_le32((uint32_t)(sizeof(*p) +
            (size_t)infs_le32_to_cpu(p->checksum_count) *
            sizeof(struct infs_data_checksum_disk)));
        status = write_object(vol, lk.block, block);
        if (status != INFS_STATUS_OK)
            return status;

        logical += chunk;
        digests += chunk;
        count -= chunk;
    }
    return INFS_STATUS_OK;
}

static int checksum_get(struct infs_volume *vol,
                        struct infs_file_payload_disk *file,
                        const uint8_t owner_id[16], uint64_t logical,
                        struct infs_data_checksum_disk *digest_out)
{
    if (!vol || !file || !owner_id || !digest_out)
        return INFS_STATUS_INVALID_ARGUMENT;

    uint8_t block[INFS_BLOCK_SIZE];
    struct infs_checksum_payload_disk *p;
    struct infs_data_checksum_disk *values;
    infs_status status = checksum_locate(vol, file, owner_id, logical, 0, NULL,
                                         block, &p, &values);
    if (status != INFS_STATUS_OK)
        return status;
    uint64_t offset = logical % INFS_CHECKSUMS_PER_OBJECT;
    if (offset >= infs_le32_to_cpu(p->checksum_count))
        return INFS_STATUS_CORRUPT;
    *digest_out = values[offset];
    return INFS_STATUS_OK;
}

static int checksum_destroy_chain(struct infs_volume *vol,
                                  struct infs_file_payload_disk *file,
                                  const uint8_t owner_id[16])
{
    if (!vol || !file || !owner_id)
        return INFS_STATUS_INVALID_ARGUMENT;

    checksum_cursor_clear_owner(vol, owner_id);
    uint8_t current[16];
    memcpy(current, file->checksum_head_id, 16);
    for (uint64_t guard = 0; !id_is_zero(current) && guard < 1048576u; ++guard) {
        struct infs_lookup lk;
        infs_status status = index_find(vol, current, &lk);
        if (status != INFS_STATUS_OK)
            return status;
        if (lk.type != INFS_OBJECT_CHECKSUM)
            return INFS_STATUS_CORRUPT;

        uint8_t block[INFS_BLOCK_SIZE];
        status = read_object(vol, lk.block, block);
        if (status != INFS_STATUS_OK)
            return status;
        struct infs_checksum_payload_disk *p;
        status = checksum_validate(block, owner_id, &p, NULL);
        if (status != INFS_STATUS_OK)
            return status;

        uint8_t next[16];
        memcpy(next, p->next_object_id, 16);
        status = index_remove(vol, current);
        if (status != INFS_STATUS_OK)
            return status;
        free_run(vol, lk.block, 1);
        if (vol->tx_error != INFS_STATUS_OK)
            return vol->tx_error;
        memcpy(current, next, 16);
    }
    if (!id_is_zero(current))
        return INFS_STATUS_LOOP_DETECTED;
    memset(file->checksum_head_id, 0, 16);
    return INFS_STATUS_OK;
}

#define INFS_MAX_FILE_EXTENTS \
    ((INFS_BLOCK_SIZE - sizeof(struct infs_object_header_disk) - \
      sizeof(struct infs_file_payload_disk)) / sizeof(struct infs_extent_disk))
