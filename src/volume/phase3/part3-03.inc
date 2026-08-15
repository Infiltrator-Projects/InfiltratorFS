// SPDX-License-Identifier: GPL-3.0-or-later
static int validate_index_entries(struct infs_volume *vol)
{
    uint8_t index_block[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    if (index_load(vol, index_block, &payload) != 0)
        return -1;
    uint32_t count = le32toh(payload->entry_count);
    struct infs_index_entry_disk *entries =
        (struct infs_index_entry_disk *)(payload + 1);
    uint64_t total = le64toh(vol->sb.total_blocks);
    int root_seen = 0;

    for (uint32_t i = 0; i < count; ++i) {
        uint64_t b = le64toh(entries[i].object_block);
        uint16_t type = le16toh(entries[i].object_type);
        if (b >= total || !bitmap_get(vol->bitmap, b) ||
            (type != INFS_OBJECT_DIRECTORY && type != INFS_OBJECT_FILE &&
             type != INFS_OBJECT_CHECKSUM)) {
            errno = EIO;
            return -1;
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (memcmp(entries[i].object_id, entries[j].object_id, 16) == 0) {
                errno = EIO;
                return -1;
            }
        }
        uint8_t object[INFS_BLOCK_SIZE];
        if (read_object(vol, b, object) != 0)
            return -1;
        struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)object;
        if (memcmp(hdr->object_id, entries[i].object_id, 16) != 0 ||
            le16toh(hdr->object_type) != type) {
            errno = EIO;
            return -1;
        }
        if (memcmp(entries[i].object_id, vol->sb.root_object_id, 16) == 0) {
            if (b != le64toh(vol->sb.root_object_block) || type != INFS_OBJECT_DIRECTORY)
                return errno = EIO, -1;
            ++root_seen;
        }
    }
    if (root_seen != 1) {
        errno = EIO;
        return -1;
    }
    return 0;
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
        uint64_t b = le64toh(vol->sb.checkpoint_block[i]);
        if (!checkpoint_matches(vol, b, &vol->sb)) {
            need_heal = 1;
            break;
        }
    }
    if (!need_heal)
        return 0;

    uint8_t raw[INFS_BLOCK_SIZE];
    if (infs_encode_superblock(raw, &vol->sb) != 0)
        return -1;
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        uint64_t b = le64toh(vol->sb.checkpoint_block[i]);
        if (write_block(vol, b, raw) != 0)
            return -1;
    }
    return fsync(vol->fd);
}

static void phase2_failpoint(const char *stage)
{
    const char *requested = getenv("INFS_TEST_CRASH_STAGE");
    if (requested && strcmp(requested, stage) == 0)
        _exit(97);
}
