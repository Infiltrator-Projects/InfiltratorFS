// SPDX-License-Identifier: GPL-3.0-or-later
static int file_validate(uint8_t block[INFS_BLOCK_SIZE],
                         struct infs_file_payload_disk **payload_out,
                         struct infs_extent_disk **extents_out)
{
    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
    if (!infs_validate_object_block(block) ||
        le16toh(hdr->object_type) != INFS_OBJECT_FILE ||
        le32toh(hdr->payload_size) < sizeof(struct infs_file_payload_disk)) {
        errno = EIO;
        return -1;
    }
    struct infs_file_payload_disk *p =
        (struct infs_file_payload_disk *)(block + sizeof(*hdr));
    if (le32toh(p->data_checksum_type) != INFS_CHECKSUM_SHA256) {
        errno = EIO;
        return -1;
    }
    uint32_t count = le32toh(p->extent_count);
    size_t need = sizeof(*p) + (size_t)count * sizeof(struct infs_extent_disk);
    if (need > le32toh(hdr->payload_size) || need > INFS_BLOCK_SIZE - sizeof(*hdr)) {
        errno = EIO;
        return -1;
    }
    struct infs_extent_disk *ext = (struct infs_extent_disk *)(p + 1);
    uint64_t next_logical = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t logical = le64toh(ext[i].logical_block);
        uint64_t physical = le64toh(ext[i].physical_block);
        uint32_t blocks = le32toh(ext[i].block_count);
        (void)physical; /* physical bounds are checked with volume context */
        if (blocks == 0 || logical != next_logical) {
            errno = EIO;
            return -1;
        }
        next_logical += blocks;
    }
    if (payload_out)
        *payload_out = p;
    if (extents_out)
        *extents_out = ext;
    return 0;
}

static int file_validate_volume(struct infs_volume *vol, uint8_t block[INFS_BLOCK_SIZE],
                                struct infs_file_payload_disk **payload_out,
                                struct infs_extent_disk **extents_out)
{
    if (file_validate(block, payload_out, extents_out) != 0)
        return -1;
    struct infs_file_payload_disk *p = *payload_out;
    struct infs_extent_disk *ext = *extents_out;
    uint32_t count = le32toh(p->extent_count);
    uint64_t total = le64toh(vol->sb.total_blocks);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t physical = le64toh(ext[i].physical_block);
        uint32_t blocks = le32toh(ext[i].block_count);
        if (physical >= total || blocks > total - physical) {
            errno = EIO;
            return -1;
        }
        for (uint32_t j = 0; j < blocks; ++j) {
            if (!bitmap_get(vol->bitmap, physical + j)) {
                errno = EIO;
                return -1;
            }
        }
    }
    return 0;
}

static uint64_t file_allocated_blocks(const struct infs_file_payload_disk *p,
                                      const struct infs_extent_disk *ext)
{
    uint32_t count = le32toh(p->extent_count);
    if (!count)
        return 0;
    const struct infs_extent_disk *last = &ext[count - 1u];
    return le64toh(last->logical_block) + le32toh(last->block_count);
}
