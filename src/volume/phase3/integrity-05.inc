// SPDX-License-Identifier: GPL-3.0-or-later
static int validate_integrity_metadata(struct infs_volume *vol)
{
    uint8_t index_block[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    if (index_load(vol, index_block, &payload) != 0)
        return INFS_STATUS_ERROR;
    uint32_t count = infs_le32_to_cpu(payload->entry_count);
    struct infs_index_entry_disk *entries =
        (struct infs_index_entry_disk *)(payload + 1);

    for (uint32_t i = 0; i < count; ++i) {
        uint16_t type = infs_le16_to_cpu(entries[i].object_type);
        if (type == INFS_OBJECT_CHECKSUM) {
            uint8_t block[INFS_BLOCK_SIZE];
            if (read_object(vol, infs_le64_to_cpu(entries[i].object_block), block) != 0)
                return INFS_STATUS_ERROR;
            struct infs_object_header_disk *hdr =
                (struct infs_object_header_disk *)block;
            struct infs_checksum_payload_disk *cp;
            if (checksum_validate(block, NULL, &cp, NULL) != 0 ||
                memcmp(hdr->parent_id, cp->owner_object_id, 16) != 0 ||
                (infs_le64_to_cpu(cp->start_logical_block) % INFS_CHECKSUMS_PER_OBJECT) != 0) {
                return INFS_STATUS_CORRUPT;
            }
            struct infs_lookup owner;
            if (index_find(vol, cp->owner_object_id, &owner) != 0 ||
                owner.type != INFS_OBJECT_FILE) {
                return INFS_STATUS_CORRUPT;
            }
            if (!id_is_zero(cp->next_object_id)) {
                struct infs_lookup next;
                uint8_t next_block[INFS_BLOCK_SIZE];
                struct infs_checksum_payload_disk *next_cp;
                if (index_find(vol, cp->next_object_id, &next) != 0 ||
                    next.type != INFS_OBJECT_CHECKSUM ||
                    read_object(vol, next.block, next_block) != 0 ||
                    checksum_validate(next_block, cp->owner_object_id,
                                      &next_cp, NULL) != 0 ||
                    infs_le64_to_cpu(next_cp->start_logical_block) <=
                        infs_le64_to_cpu(cp->start_logical_block)) {
                    return INFS_STATUS_CORRUPT;
                }
            }
        } else if (type == INFS_OBJECT_FILE) {
            uint8_t object[INFS_BLOCK_SIZE];
            if (read_object(vol, infs_le64_to_cpu(entries[i].object_block), object) != 0)
                return INFS_STATUS_ERROR;
            struct infs_object_header_disk *hdr =
                (struct infs_object_header_disk *)object;
            struct infs_file_payload_disk *file;
            struct infs_extent_disk *ext;
            if (file_validate_volume(vol, object, &file, &ext) != 0)
                return INFS_STATUS_ERROR;
            uint64_t allocated = file_allocated_blocks(file, ext);
            if (allocated && id_is_zero(file->checksum_head_id)) {
                return INFS_STATUS_CORRUPT;
            }
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
                    int status = checksum_get(vol, file, hdr->object_id,
                                              logical + chunk - 1u, &digest);
                    if (status != 0)
                        return status;
                    logical += chunk;
                    remaining -= chunk;
                }
            }
        }
    }
    return 0;
}
