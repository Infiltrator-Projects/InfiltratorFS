// SPDX-License-Identifier: GPL-3.0-or-later
static int ownership_claim(uint8_t *owners, uint64_t total, uint64_t block)
{
    if (block >= total || bitmap_get(owners, block))
        return INFS_STATUS_CORRUPT;
    bitmap_set(owners, block, 1);
    return INFS_STATUS_OK;
}

static int ownership_claim_data(uint8_t *owners, uint8_t *data_owners,
                                uint64_t total, uint64_t block,
                                int allow_shared)
{
    if (block >= total)
        return INFS_STATUS_CORRUPT;
    if (bitmap_get(owners, block)) {
        if (!allow_shared || !bitmap_get(data_owners, block))
            return INFS_STATUS_CORRUPT;
        return INFS_STATUS_OK;
    }
    bitmap_set(owners, block, 1);
    bitmap_set(data_owners, block, 1);
    return INFS_STATUS_OK;
}

static int validate_bitmap_ownership(struct infs_volume *vol,
                                     const struct infs_index_entry_disk *entries,
                                     uint32_t count)
{
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    uint8_t *owners = calloc(1, vol->bitmap_bytes);
    uint8_t *data_owners = calloc(1, vol->bitmap_bytes);
    if (!owners || !data_owners) {
        free(owners);
        free(data_owners);
        return INFS_STATUS_NO_MEMORY;
    }
    int allow_shared =
        (infs_le64_to_cpu(vol->sb.incompat_flags) &
         INFS_INCOMPAT_SHARED_EXTENTS) != 0;

    int status = INFS_STATUS_OK;
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        status = ownership_claim(
            owners, total, infs_le64_to_cpu(vol->sb.checkpoint_block[i]));
        if (status != INFS_STATUS_OK)
            goto out;
    }

    uint64_t bitmap_start = infs_le64_to_cpu(vol->sb.bitmap_start_block);
    uint64_t bitmap_blocks = infs_le64_to_cpu(vol->sb.bitmap_block_count);
    for (uint64_t i = 0; i < bitmap_blocks; ++i) {
        status = ownership_claim(owners, total, bitmap_start + i);
        if (status != INFS_STATUS_OK)
            goto out;
    }

    status = ownership_claim(
        owners, total, infs_le64_to_cpu(vol->sb.object_index_block));
    if (status != INFS_STATUS_OK)
        goto out;

    for (uint32_t i = 0; i < count; ++i) {
        uint64_t object_block = infs_le64_to_cpu(entries[i].object_block);
        status = ownership_claim(owners, total, object_block);
        if (status != INFS_STATUS_OK)
            goto out;

        if (infs_le16_to_cpu(entries[i].object_type) != INFS_OBJECT_FILE)
            continue;

        uint8_t object[INFS_BLOCK_SIZE];
        status = read_object(vol, object_block, object);
        if (status != INFS_STATUS_OK)
            goto out;

        struct infs_file_payload_disk *file;
        struct infs_extent_disk *ext;
        status = file_validate_volume(vol, object, &file, &ext);
        if (status != INFS_STATUS_OK)
            goto out;

        uint32_t extent_count = infs_le32_to_cpu(file->extent_count);
        for (uint32_t j = 0; j < extent_count; ++j) {
            if (infs_le32_to_cpu(ext[j].flags) != INFS_EXTENT_NORMAL)
                continue;
            uint64_t physical = infs_le64_to_cpu(ext[j].physical_block);
            uint32_t blocks = infs_le32_to_cpu(ext[j].block_count);
            for (uint32_t k = 0; k < blocks; ++k) {
                status = ownership_claim_data(
                    owners, data_owners, total, physical + k, allow_shared);
                if (status != INFS_STATUS_OK)
                    goto out;
            }
        }
    }

    for (uint64_t b = 0; b < total; ++b) {
        if (bitmap_get(owners, b) != bitmap_get(vol->bitmap, b)) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }
    }

out:
    free(data_owners);
    free(owners);
    return status;
}

static int validate_integrity_metadata(struct infs_volume *vol)
{
    uint8_t index_block[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    int status = index_load(vol, index_block, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    uint32_t count = infs_le32_to_cpu(payload->entry_count);
    struct infs_index_entry_disk *entries =
        (struct infs_index_entry_disk *)(payload + 1);

    status = validate_bitmap_ownership(vol, entries, count);
    if (status != INFS_STATUS_OK)
        return status;

    for (uint32_t i = 0; i < count; ++i) {
        uint16_t type = infs_le16_to_cpu(entries[i].object_type);
        if (type == INFS_OBJECT_CHECKSUM) {
            uint8_t block[INFS_BLOCK_SIZE];
            status = read_object(
                vol, infs_le64_to_cpu(entries[i].object_block), block);
            if (status != INFS_STATUS_OK)
                return status;
            struct infs_object_header_disk *hdr =
                (struct infs_object_header_disk *)block;
            struct infs_checksum_payload_disk *cp;
            if (checksum_validate(block, NULL, &cp, NULL) != 0 ||
                memcmp(hdr->parent_id, cp->owner_object_id, 16) != 0 ||
                (infs_le64_to_cpu(cp->start_logical_block) %
                 INFS_CHECKSUMS_PER_OBJECT) != 0) {
                return INFS_STATUS_CORRUPT;
            }
            struct infs_lookup owner;
            status = index_find(vol, cp->owner_object_id, &owner);
            if (status != INFS_STATUS_OK || owner.type != INFS_OBJECT_FILE)
                return INFS_STATUS_CORRUPT;
            if (!id_is_zero(cp->next_object_id)) {
                struct infs_lookup next;
                uint8_t next_block[INFS_BLOCK_SIZE];
                struct infs_checksum_payload_disk *next_cp;
                status = index_find(vol, cp->next_object_id, &next);
                if (status != INFS_STATUS_OK ||
                    next.type != INFS_OBJECT_CHECKSUM)
                    return INFS_STATUS_CORRUPT;
                status = read_object(vol, next.block, next_block);
                if (status != INFS_STATUS_OK)
                    return status;
                if (checksum_validate(next_block, cp->owner_object_id,
                                      &next_cp, NULL) != 0 ||
                    infs_le64_to_cpu(next_cp->start_logical_block) <=
                        infs_le64_to_cpu(cp->start_logical_block)) {
                    return INFS_STATUS_CORRUPT;
                }
            }
        } else if (type == INFS_OBJECT_FILE) {
            uint8_t object[INFS_BLOCK_SIZE];
            status = read_object(
                vol, infs_le64_to_cpu(entries[i].object_block), object);
            if (status != INFS_STATUS_OK)
                return status;
            struct infs_object_header_disk *hdr =
                (struct infs_object_header_disk *)object;
            struct infs_file_payload_disk *file;
            struct infs_extent_disk *ext;
            status = file_validate_volume(vol, object, &file, &ext);
            if (status != INFS_STATUS_OK)
                return status;
            uint64_t allocated = file_allocated_blocks(file, ext);
            if (allocated && id_is_zero(file->checksum_head_id))
                return INFS_STATUS_CORRUPT;
            uint32_t extent_count = infs_le32_to_cpu(file->extent_count);
            for (uint32_t j = 0; j < extent_count; ++j) {
                if (infs_le32_to_cpu(ext[j].flags) != INFS_EXTENT_NORMAL)
                    continue;
                uint64_t logical = infs_le64_to_cpu(ext[j].logical_block);
                uint64_t remaining = infs_le32_to_cpu(ext[j].block_count);
                while (remaining) {
                    uint64_t chunk = INFS_CHECKSUMS_PER_OBJECT -
                        (logical % INFS_CHECKSUMS_PER_OBJECT);
                    if (chunk > remaining)
                        chunk = remaining;
                    struct infs_data_checksum_disk digest;
                    status = checksum_get(vol, file, hdr->object_id,
                                          logical + chunk - 1u, &digest);
                    if (status != INFS_STATUS_OK)
                        return status;
                    logical += chunk;
                    remaining -= chunk;
                }
            }
        } else if (type == INFS_OBJECT_DIRECTORY) {
            uint8_t object[INFS_BLOCK_SIZE];
            status = read_object(
                vol, infs_le64_to_cpu(entries[i].object_block), object);
            if (status != INFS_STATUS_OK)
                return status;
            status = dir_validate(object, NULL);
            if (status != INFS_STATUS_OK)
                return status;
        }
    }
    return INFS_STATUS_OK;
}
