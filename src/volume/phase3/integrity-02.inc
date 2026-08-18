// SPDX-License-Identifier: GPL-3.0-or-later
static int checksum_locate(struct infs_volume *vol,
                           struct infs_file_payload_disk *file,
                           const uint8_t owner_id[16], uint64_t logical,
                           int create, struct infs_lookup *lookup_out,
                           uint8_t block_out[INFS_BLOCK_SIZE],
                           struct infs_checksum_payload_disk **payload_out,
                           struct infs_data_checksum_disk **checksums_out)
{
    if (!vol || !file || !owner_id ||
        ((payload_out || checksums_out) && !block_out))
        return INFS_STATUS_INVALID_ARGUMENT;

    uint64_t target_start =
        (logical / INFS_CHECKSUMS_PER_OBJECT) * INFS_CHECKSUMS_PER_OBJECT;
    uint8_t current_id[16];
    uint8_t previous_id[16] = {0};
    uint64_t previous_start = 0;
    int have_previous = 0;
    infs_status status;

    if (id_is_zero(file->checksum_head_id)) {
        if (!create)
            return INFS_STATUS_CORRUPT;
        status = checksum_create_object(vol, owner_id, target_start, NULL,
                                        current_id);
        if (status != INFS_STATUS_OK)
            return status;
        memcpy(file->checksum_head_id, current_id, 16);
    } else {
        memcpy(current_id, file->checksum_head_id, 16);
    }

    for (uint64_t guard = 0; guard < 1048576u; ++guard) {
        struct infs_lookup lk;
        status = index_find(vol, current_id, &lk);
        if (status != INFS_STATUS_OK)
            return status;
        if (lk.type != INFS_OBJECT_CHECKSUM)
            return INFS_STATUS_CORRUPT;

        uint8_t block[INFS_BLOCK_SIZE];
        status = read_object(vol, lk.block, block);
        if (status != INFS_STATUS_OK)
            return status;

        struct infs_checksum_payload_disk *p;
        struct infs_data_checksum_disk *values;
        status = checksum_validate(block, owner_id, &p, &values);
        if (status != INFS_STATUS_OK)
            return status;

        uint64_t start = infs_le64_to_cpu(p->start_logical_block);
        if ((start % INFS_CHECKSUMS_PER_OBJECT) != 0 ||
            (have_previous && start <= previous_start))
            return INFS_STATUS_CORRUPT;

        if (start == target_start) {
            if (lookup_out)
                *lookup_out = lk;
            if (block_out)
                memcpy(block_out, block, INFS_BLOCK_SIZE);
            if (payload_out) {
                *payload_out = (struct infs_checksum_payload_disk *)(
                    block_out + sizeof(struct infs_object_header_disk));
            }
            if (checksums_out) {
                struct infs_checksum_payload_disk *copy =
                    (struct infs_checksum_payload_disk *)(
                        block_out + sizeof(struct infs_object_header_disk));
                *checksums_out = (struct infs_data_checksum_disk *)(copy + 1);
            }
            return INFS_STATUS_OK;
        }

        if (start > target_start) {
            if (!create)
                return INFS_STATUS_CORRUPT;

            uint8_t inserted_id[16];
            status = checksum_create_object(vol, owner_id, target_start,
                                            current_id, inserted_id);
            if (status != INFS_STATUS_OK)
                return status;
            if (!have_previous) {
                memcpy(file->checksum_head_id, inserted_id, 16);
            } else {
                struct infs_lookup previous_lk;
                uint8_t previous_block[INFS_BLOCK_SIZE];
                struct infs_checksum_payload_disk *previous;
                status = index_find(vol, previous_id, &previous_lk);
                if (status != INFS_STATUS_OK)
                    return status;
                if (previous_lk.type != INFS_OBJECT_CHECKSUM)
                    return INFS_STATUS_CORRUPT;
                status = read_object(vol, previous_lk.block, previous_block);
                if (status != INFS_STATUS_OK)
                    return status;
                status = checksum_validate(previous_block, owner_id, &previous,
                                           NULL);
                if (status != INFS_STATUS_OK)
                    return status;
                memcpy(previous->next_object_id, inserted_id, 16);
                status = write_object(vol, previous_lk.block, previous_block);
                if (status != INFS_STATUS_OK)
                    return status;
            }
            memcpy(current_id, inserted_id, 16);
            continue;
        }

        uint8_t next_id[16];
        memcpy(next_id, p->next_object_id, 16);
        if (id_is_zero(next_id)) {
            if (!create)
                return INFS_STATUS_CORRUPT;

            uint8_t inserted_id[16];
            status = checksum_create_object(vol, owner_id, target_start, NULL,
                                            inserted_id);
            if (status != INFS_STATUS_OK)
                return status;
            memcpy(p->next_object_id, inserted_id, 16);
            status = write_object(vol, lk.block, block);
            if (status != INFS_STATUS_OK)
                return status;
            memcpy(previous_id, current_id, 16);
            previous_start = start;
            have_previous = 1;
            memcpy(current_id, inserted_id, 16);
            continue;
        }

        memcpy(previous_id, current_id, 16);
        previous_start = start;
        have_previous = 1;
        memcpy(current_id, next_id, 16);
    }

    return INFS_STATUS_LOOP_DETECTED;
}
