// SPDX-License-Identifier: GPL-3.0-or-later
static int checksum_locate(struct infs_volume *vol,
                           struct infs_file_payload_disk *file,
                           const uint8_t owner_id[16], uint64_t logical,
                           int create, struct infs_lookup *lookup_out,
                           uint8_t block_out[INFS_BLOCK_SIZE],
                           struct infs_checksum_payload_disk **payload_out,
                           struct infs_data_checksum_disk **checksums_out)
{
    uint64_t segment = logical / INFS_CHECKSUMS_PER_OBJECT;
    uint64_t expected_start = 0;
    uint8_t current_id[16];

    if (id_is_zero(file->checksum_head_id)) {
        if (!create) {
            return INFS_STATUS_CORRUPT;
        }
        if (checksum_create_object(vol, owner_id, 0, current_id) != 0)
            return INFS_STATUS_ERROR;
        memcpy(file->checksum_head_id, current_id, 16);
    } else {
        memcpy(current_id, file->checksum_head_id, 16);
    }

    for (uint64_t n = 0; n <= segment; ++n) {
        struct infs_lookup lk;
        if (index_find(vol, current_id, &lk) != 0)
            return INFS_STATUS_ERROR;
        if (lk.type != INFS_OBJECT_CHECKSUM) {
            return INFS_STATUS_CORRUPT;
        }
        uint8_t block[INFS_BLOCK_SIZE];
        if (read_object(vol, lk.block, block) != 0)
            return INFS_STATUS_ERROR;
        struct infs_checksum_payload_disk *p;
        struct infs_data_checksum_disk *values;
        if (checksum_validate(block, owner_id, &p, &values) != 0 ||
            infs_le64_to_cpu(p->start_logical_block) != expected_start) {
            return INFS_STATUS_CORRUPT;
        }
        if (n == segment) {
            if (lookup_out)
                *lookup_out = lk;
            if (block_out)
                memcpy(block_out, block, INFS_BLOCK_SIZE);
            if (payload_out) {
                struct infs_checksum_payload_disk *copy =
                    (struct infs_checksum_payload_disk *)(block_out + sizeof(struct infs_object_header_disk));
                *payload_out = copy;
            }
            if (checksums_out) {
                struct infs_checksum_payload_disk *copy =
                    (struct infs_checksum_payload_disk *)(block_out + sizeof(struct infs_object_header_disk));
                *checksums_out = (struct infs_data_checksum_disk *)(copy + 1);
            }
            return 0;
        }

        if (id_is_zero(p->next_object_id)) {
            if (!create) {
                return INFS_STATUS_CORRUPT;
            }
            uint8_t next_id[16];
            if (checksum_create_object(vol, owner_id,
                                       expected_start + INFS_CHECKSUMS_PER_OBJECT,
                                       next_id) != 0)
                return INFS_STATUS_ERROR;
            memcpy(p->next_object_id, next_id, 16);
            if (write_object(vol, lk.block, block) != 0)
                return INFS_STATUS_ERROR;
            memcpy(current_id, next_id, 16);
        } else {
            memcpy(current_id, p->next_object_id, 16);
        }
        expected_start += INFS_CHECKSUMS_PER_OBJECT;
    }

    return INFS_STATUS_CORRUPT;
}
