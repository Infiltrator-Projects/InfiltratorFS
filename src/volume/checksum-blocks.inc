// SPDX-License-Identifier: GPL-3.0-or-later
/* Phase 3 data integrity helpers. Data checksums are stored in hidden,
 * copy-on-write checksum objects addressed through the persistent object
 * index. Each checksum authenticates one complete 4096-byte logical block. */

static void data_block_checksum(const uint8_t block[INFS_BLOCK_SIZE],
                                struct infs_data_checksum_disk *out)
{
    infs_sha256(block, INFS_BLOCK_SIZE, out->bytes);
}

static int checksum_validate(uint8_t block[INFS_BLOCK_SIZE],
                             const uint8_t owner_id[16],
                             struct infs_checksum_payload_disk **payload_out,
                             struct infs_data_checksum_disk **checksums_out)
{
    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
    if (!infs_validate_object_block(block) ||
        infs_le16_to_cpu(hdr->object_type) != INFS_OBJECT_CHECKSUM ||
        infs_le32_to_cpu(hdr->payload_size) < sizeof(struct infs_checksum_payload_disk)) {
        return INFS_STATUS_CORRUPT;
    }
    struct infs_checksum_payload_disk *p =
        (struct infs_checksum_payload_disk *)(block + sizeof(*hdr));
    uint32_t count = infs_le32_to_cpu(p->checksum_count);
    if (count > INFS_CHECKSUMS_PER_OBJECT)
        return INFS_STATUS_CORRUPT;
    size_t need = sizeof(*p) + (size_t)count * sizeof(struct infs_data_checksum_disk);
    if (need != infs_le32_to_cpu(hdr->payload_size) ||
        need > INFS_BLOCK_SIZE - sizeof(*hdr) ||
        infs_le32_to_cpu(p->reserved) != 0 || id_is_zero(p->owner_object_id)) {
        return INFS_STATUS_CORRUPT;
    }
    if (owner_id && memcmp(p->owner_object_id, owner_id, 16) != 0) {
        return INFS_STATUS_CORRUPT;
    }
    if (payload_out)
        *payload_out = p;
    if (checksums_out)
        *checksums_out = (struct infs_data_checksum_disk *)(p + 1);
    return INFS_STATUS_OK;
}

static int checksum_create_object(struct infs_volume *vol,
                                  const uint8_t owner_id[16],
                                  uint64_t start_logical,
                                  const uint8_t next_id[16],
                                  uint8_t id_out[16])
{
    uint64_t block_no = 0, got = 0;
    infs_status status = alloc_run(vol, 1, &block_no, &got);
    if (status != INFS_STATUS_OK)
        return status;
    if (got != 1)
        return INFS_STATUS_CORRUPT;

    uint8_t id[16];
    status = generate_unique_object_id(vol, id);
    if (status != INFS_STATUS_OK) {
        free_run(vol, block_no, 1);
        return status;
    }

    uint8_t block[INFS_BLOCK_SIZE];
    status = infs_object_init(block, INFS_OBJECT_CHECKSUM, id, owner_id,
                              infs_le64_to_cpu(vol->sb.generation) + 1u,
                              sizeof(struct infs_checksum_payload_disk));
    if (status != INFS_STATUS_OK) {
        free_run(vol, block_no, 1);
        return status;
    }
    struct infs_checksum_payload_disk *p =
        (struct infs_checksum_payload_disk *)(block + sizeof(struct infs_object_header_disk));
    memcpy(p->owner_object_id, owner_id, 16);
    p->start_logical_block = infs_cpu_to_le64(start_logical);
    p->checksum_count = infs_cpu_to_le32(0);
    p->reserved = 0;
    if (next_id)
        memcpy(p->next_object_id, next_id, 16);
    status = infs_object_finalize(block);
    if (status == INFS_STATUS_OK)
        status = write_block(vol, block_no, block);
    if (status != INFS_STATUS_OK) {
        free_run(vol, block_no, 1);
        return status;
    }
    status = index_add(vol, id, block_no, INFS_OBJECT_CHECKSUM);
    if (status != INFS_STATUS_OK) {
        free_run(vol, block_no, 1);
        return status;
    }
    memcpy(id_out, id, 16);
    return INFS_STATUS_OK;
}
