// SPDX-License-Identifier: GPL-3.0-or-later
static int validate_index_entries(struct infs_volume *vol)
{
    uint8_t index_block[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    int status = index_load(vol, index_block, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_object_header_disk *index_header =
        (struct infs_object_header_disk *)index_block;
    uint32_t count = infs_le32_to_cpu(payload->entry_count);
    size_t expected_payload = sizeof(*payload) +
        (size_t)count * sizeof(struct infs_index_entry_disk);
    if (infs_le32_to_cpu(payload->reserved) != 0 ||
        expected_payload != infs_le32_to_cpu(index_header->payload_size) ||
        !id_is_zero(index_header->parent_id))
        return INFS_STATUS_CORRUPT;

    struct infs_index_entry_disk *entries =
        (struct infs_index_entry_disk *)(payload + 1);
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    int root_seen = 0;

    for (uint32_t i = 0; i < count; ++i) {
        uint64_t b = infs_le64_to_cpu(entries[i].object_block);
        uint16_t type = infs_le16_to_cpu(entries[i].object_type);
        if (id_is_zero(entries[i].object_id) || b >= total ||
            !bitmap_get(vol->bitmap, b) ||
            infs_le16_to_cpu(entries[i].flags) != 0 ||
            infs_le32_to_cpu(entries[i].reserved) != 0 ||
            (type != INFS_OBJECT_DIRECTORY && type != INFS_OBJECT_FILE &&
             type != INFS_OBJECT_CHECKSUM)) {
            return INFS_STATUS_CORRUPT;
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (memcmp(entries[i].object_id, entries[j].object_id, 16) == 0)
                return INFS_STATUS_CORRUPT;
        }
        uint8_t object[INFS_BLOCK_SIZE];
        status = read_object(vol, b, object);
        if (status != INFS_STATUS_OK)
            return status;
        struct infs_object_header_disk *hdr =
            (struct infs_object_header_disk *)object;
        if (memcmp(hdr->object_id, entries[i].object_id, 16) != 0 ||
            infs_le16_to_cpu(hdr->object_type) != type)
            return INFS_STATUS_CORRUPT;
        if (memcmp(entries[i].object_id, vol->sb.root_object_id, 16) == 0) {
            if (b != infs_le64_to_cpu(vol->sb.root_object_block) ||
                type != INFS_OBJECT_DIRECTORY)
                return INFS_STATUS_CORRUPT;
            ++root_seen;
        }
    }
    if (root_seen != 1)
        return INFS_STATUS_CORRUPT;
    return INFS_STATUS_OK;
}

static int checkpoint_matches(struct infs_volume *vol, uint64_t block_no,
                              const struct infs_superblock_disk *best)
{
    uint8_t raw[INFS_BLOCK_SIZE];
    if (read_block(vol, block_no, raw) != 0)
        return 0;
    struct infs_superblock_disk sb;
    if (infs_decode_superblock(raw, &sb) != 0)
        return 0;
    return memcmp(&sb, best, sizeof(sb)) == 0;
}

static int heal_checkpoints(struct infs_volume *vol)
{
    int need_heal = 0;
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        uint64_t b = infs_le64_to_cpu(vol->sb.checkpoint_block[i]);
        if (!checkpoint_matches(vol, b, &vol->sb)) {
            need_heal = 1;
            break;
        }
    }
    if (!need_heal)
        return INFS_STATUS_OK;

    uint8_t raw[INFS_BLOCK_SIZE];
    infs_status status = infs_encode_superblock(raw, &vol->sb);
    if (status != INFS_STATUS_OK)
        return status;
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        uint64_t b = infs_le64_to_cpu(vol->sb.checkpoint_block[i]);
        status = write_block(vol, b, raw);
        if (status != INFS_STATUS_OK)
            return status;
    }
    return infs_storage_flush(&vol->storage);
}

static void phase2_failpoint(const char *stage)
{
    const char *requested = getenv("INFS_TEST_CRASH_STAGE");
    if (requested && strcmp(requested, stage) == 0)
        _Exit(97);
}
