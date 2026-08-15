// SPDX-License-Identifier: GPL-3.0-or-later
static int validate_integrity_metadata(struct infs_volume *vol)
{
    uint8_t index_block[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    if (index_load(vol, index_block, &payload) != 0)
        return -1;
    uint32_t count = le32toh(payload->entry_count);
    struct infs_index_entry_disk *entries =
        (struct infs_index_entry_disk *)(payload + 1);

    for (uint32_t i = 0; i < count; ++i) {
        uint16_t type = le16toh(entries[i].object_type);
        if (type == INFS_OBJECT_CHECKSUM) {
            uint8_t block[INFS_BLOCK_SIZE];
            if (read_object(vol, le64toh(entries[i].object_block), block) != 0)
                return -1;
            struct infs_object_header_disk *hdr =
                (struct infs_object_header_disk *)block;
            struct infs_checksum_payload_disk *cp;
            if (checksum_validate(block, NULL, &cp, NULL) != 0 ||
                memcmp(hdr->parent_id, cp->owner_object_id, 16) != 0 ||
                (le64toh(cp->start_logical_block) % INFS_CHECKSUMS_PER_OBJECT) != 0) {
                errno = EIO;
                return -1;
            }
            struct infs_lookup owner;
            if (index_find(vol, cp->owner_object_id, &owner) != 0 ||
                owner.type != INFS_OBJECT_FILE) {
                errno = EIO;
                return -1;
            }
            if (!id_is_zero(cp->next_object_id)) {
                struct infs_lookup next;
                if (index_find(vol, cp->next_object_id, &next) != 0 ||
                    next.type != INFS_OBJECT_CHECKSUM) {
                    errno = EIO;
                    return -1;
                }
            }
        } else if (type == INFS_OBJECT_FILE) {
            uint8_t object[INFS_BLOCK_SIZE];
            if (read_object(vol, le64toh(entries[i].object_block), object) != 0)
                return -1;
            struct infs_object_header_disk *hdr =
                (struct infs_object_header_disk *)object;
            struct infs_file_payload_disk *file;
            struct infs_extent_disk *ext;
            if (file_validate_volume(vol, object, &file, &ext) != 0)
                return -1;
            uint64_t blocks = file_allocated_blocks(file, ext);
            if (blocks && id_is_zero(file->checksum_head_id)) {
                errno = EIO;
                return -1;
            }
            for (uint64_t first = 0; first < blocks; first += INFS_CHECKSUMS_PER_OBJECT) {
                uint64_t last = first + INFS_CHECKSUMS_PER_OBJECT - 1u;
                if (last >= blocks) last = blocks - 1u;
                struct infs_data_checksum_disk digest;
                if (checksum_get(vol, file, hdr->object_id, last, &digest) != 0)
                    return -1;
            }
        }
    }
    return 0;
}
