// SPDX-License-Identifier: GPL-3.0-or-later
static struct infs_data_checksum_disk *file_inline_digest(
    struct infs_file_payload_disk *file)
{
    return (struct infs_data_checksum_disk *)(file + 1);
}

static uint8_t *file_inline_bytes(struct infs_file_payload_disk *file)
{
    return (uint8_t *)(file_inline_digest(file) + 1);
}

static void file_inline_checksum(const uint8_t *data, size_t size,
                                 struct infs_data_checksum_disk *out)
{
    uint8_t block[INFS_BLOCK_SIZE] = {0};
    if (size)
        memcpy(block, data, size);
    infs_sha256(block, sizeof(block), out->bytes);
}

static int file_is_inline(const uint8_t block[INFS_BLOCK_SIZE],
                          const struct infs_file_payload_disk *file)
{
    const struct infs_object_header_disk *hdr =
        (const struct infs_object_header_disk *)block;
    if (infs_le16_to_cpu(hdr->object_version) != INFS_OBJECT_VERSION_CLASSIC)
        return 0;
    uint64_t size = infs_le64_to_cpu(file->attributes.logical_size);
    if (size == 0 || infs_le32_to_cpu(file->extent_count) != 0)
        return 0;
    uint64_t expected = sizeof(*file) + sizeof(struct infs_data_checksum_disk) +
        size;
    return size <= INFS_INLINE_DATA_MAX &&
           expected == infs_le32_to_cpu(hdr->payload_size);
}

static int file_validate(uint8_t block[INFS_BLOCK_SIZE],
                         struct infs_file_payload_disk **payload_out,
                         struct infs_extent_disk **extents_out)
{
    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
    if (!infs_validate_object_block(block) ||
        infs_le16_to_cpu(hdr->object_type) != INFS_OBJECT_FILE ||
        infs_le32_to_cpu(hdr->payload_size) < sizeof(struct infs_file_payload_disk))
        return INFS_STATUS_CORRUPT;
    uint16_t version = infs_le16_to_cpu(hdr->object_version);
    if (version != INFS_OBJECT_VERSION_CLASSIC &&
        version != INFS_OBJECT_VERSION_PAGED)
        return INFS_STATUS_CORRUPT;
    struct infs_file_payload_disk *p =
        (struct infs_file_payload_disk *)(block + sizeof(*hdr));
    if (validate_common_metadata(&p->attributes, &p->posix) != INFS_STATUS_OK ||
        infs_le32_to_cpu(p->data_checksum_type) != INFS_CHECKSUM_SHA256)
        return INFS_STATUS_CORRUPT;
    uint32_t count = infs_le32_to_cpu(p->extent_count);
    uint64_t logical_size = infs_le64_to_cpu(p->attributes.logical_size);
    uint32_t payload_size = infs_le32_to_cpu(hdr->payload_size);
    struct infs_extent_disk *ext = (struct infs_extent_disk *)(p + 1);

    if (version == INFS_OBJECT_VERSION_PAGED) {
        if (!count || logical_size == 0)
            return INFS_STATUS_CORRUPT;
        struct infs_extent_head_disk *head = file_extent_head(p);
        uint32_t pages = infs_le32_to_cpu(head->page_count);
        if (!pages || pages > INFS_EXTENT_PAGE_POINTERS ||
            infs_le32_to_cpu(head->reserved) != 0)
            return INFS_STATUS_CORRUPT;
        size_t need = sizeof(*p) + sizeof(*head) +
            (size_t)pages * sizeof(uint64_t);
        if (need != payload_size || need > INFS_BLOCK_SIZE - sizeof(*hdr))
            return INFS_STATUS_CORRUPT;
        if (payload_out)
            *payload_out = p;
        if (extents_out)
            *extents_out = NULL;
        return INFS_STATUS_OK;
    }

    if (count == 0) {
        if (logical_size == 0) {
            if (payload_size != sizeof(*p) || !id_is_zero(p->checksum_head_id))
                return INFS_STATUS_CORRUPT;
        } else {
            uint64_t expected = sizeof(*p) +
                sizeof(struct infs_data_checksum_disk) + logical_size;
            if (logical_size > INFS_INLINE_DATA_MAX || expected != payload_size ||
                !id_is_zero(p->checksum_head_id))
                return INFS_STATUS_CORRUPT;
            struct infs_data_checksum_disk actual;
            file_inline_checksum(file_inline_bytes(p), (size_t)logical_size,
                                 &actual);
            if (memcmp(file_inline_digest(p), &actual, sizeof(actual)) != 0)
                return INFS_STATUS_CORRUPT;
        }
        if (payload_out)
            *payload_out = p;
        if (extents_out)
            *extents_out = ext;
        return INFS_STATUS_OK;
    }

    const size_t max_extents =
        (INFS_BLOCK_SIZE - sizeof(*hdr) - sizeof(*p)) /
        sizeof(struct infs_extent_disk);
    if ((size_t)count > max_extents)
        return INFS_STATUS_CORRUPT;
    size_t need = sizeof(*p) + (size_t)count * sizeof(struct infs_extent_disk);
    if (need != payload_size || need > INFS_BLOCK_SIZE - sizeof(*hdr))
        return INFS_STATUS_CORRUPT;
    uint64_t next_logical = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t logical = infs_le64_to_cpu(ext[i].logical_block);
        uint64_t physical = infs_le64_to_cpu(ext[i].physical_block);
        uint32_t blocks = infs_le32_to_cpu(ext[i].block_count);
        uint32_t flags = infs_le32_to_cpu(ext[i].flags);
        if (!blocks || logical != next_logical || logical > UINT64_MAX - blocks ||
            !extent_flags_valid(blocks, physical, flags))
            return INFS_STATUS_CORRUPT;
        next_logical += blocks;
    }
    uint64_t required_blocks = logical_size / INFS_BLOCK_SIZE +
        ((logical_size % INFS_BLOCK_SIZE) != 0);
    if (next_logical != required_blocks)
        return INFS_STATUS_CORRUPT;
    if (payload_out)
        *payload_out = p;
    if (extents_out)
        *extents_out = ext;
    return INFS_STATUS_OK;
}

static int file_validate_volume(struct infs_volume *vol,
                                uint8_t block[INFS_BLOCK_SIZE],
                                struct infs_file_payload_disk **payload_out,
                                struct infs_extent_disk **extents_out)
{
    struct infs_file_payload_disk *p;
    struct infs_extent_disk *ext;
    int status = file_validate(block, &p, &ext);
    if (status != INFS_STATUS_OK)
        return status;
    if (file_is_inline(block, p) &&
        (infs_le64_to_cpu(vol->sb.incompat_flags) & INFS_INCOMPAT_INLINE_DATA) == 0)
        return INFS_STATUS_CORRUPT;
    if (file_has_paged_extents(block)) {
        status = paged_extent_head_validate(vol, block, p);
        if (status != INFS_STATUS_OK)
            return status;
        if (payload_out)
            *payload_out = p;
        if (extents_out)
            *extents_out = NULL;
        return INFS_STATUS_OK;
    }
    uint32_t count = infs_le32_to_cpu(p->extent_count);
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t physical = infs_le64_to_cpu(ext[i].physical_block);
        uint32_t blocks = infs_le32_to_cpu(ext[i].block_count);
        uint32_t flags = infs_le32_to_cpu(ext[i].flags);
        uint64_t physical_blocks;
        if (extent_kind(flags) == INFS_EXTENT_HOLE)
            continue;
        if (extent_is_compressed(flags) &&
            (infs_le64_to_cpu(vol->sb.incompat_flags) &
             INFS_INCOMPAT_COMPRESSED_EXTENTS) == 0)
            return INFS_STATUS_CORRUPT;
        physical_blocks = extent_physical_blocks(blocks, flags);
        if (!physical_blocks || physical >= total ||
            physical_blocks > total - physical ||
            !bitmap_get(vol->bitmap, physical) ||
            !bitmap_get(vol->bitmap, physical + physical_blocks - 1u))
            return INFS_STATUS_CORRUPT;
    }
    if (payload_out)
        *payload_out = p;
    if (extents_out)
        *extents_out = ext;
    return INFS_STATUS_OK;
}

static uint64_t file_logical_blocks_classic(const struct infs_file_payload_disk *p,
                                    const struct infs_extent_disk *ext)
{
    uint32_t count = infs_le32_to_cpu(p->extent_count);
    if (!count)
        return 0;
    const struct infs_extent_disk *last = &ext[count - 1u];
    return infs_le64_to_cpu(last->logical_block) + infs_le32_to_cpu(last->block_count);
}

static uint64_t file_allocated_blocks_classic(const struct infs_file_payload_disk *p,
                                      const struct infs_extent_disk *ext)
{
    uint64_t blocks = 0;
    uint32_t count = infs_le32_to_cpu(p->extent_count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t logical = infs_le32_to_cpu(ext[i].block_count);
        uint32_t flags = infs_le32_to_cpu(ext[i].flags);
        blocks += extent_physical_blocks(logical, flags);
    }
    return blocks;
}


static int file_logical_blocks_volume(struct infs_volume *vol,
                                      uint8_t object[INFS_BLOCK_SIZE],
                                      struct infs_file_payload_disk *file,
                                      struct infs_extent_disk *classic,
                                      uint64_t *blocks_out)
{
    if (!blocks_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (!file_has_paged_extents(object)) {
        *blocks_out = file_logical_blocks_classic(file, classic);
        return INFS_STATUS_OK;
    }
    struct infs_extent_disk *all = NULL;
    uint32_t count = 0;
    infs_status status = paged_extent_snapshot(vol, object, file, &all, &count);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_extent_disk *last = &all[count - 1u];
    *blocks_out = infs_le64_to_cpu(last->logical_block) +
        infs_le32_to_cpu(last->block_count);
    free(all);
    return INFS_STATUS_OK;
}

static int file_allocated_blocks_volume(struct infs_volume *vol,
                                        uint8_t object[INFS_BLOCK_SIZE],
                                        struct infs_file_payload_disk *file,
                                        struct infs_extent_disk *classic,
                                        uint64_t *blocks_out)
{
    if (!blocks_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (!file_has_paged_extents(object)) {
        *blocks_out = file_allocated_blocks_classic(file, classic);
        return INFS_STATUS_OK;
    }
    struct infs_extent_disk *all = NULL;
    uint32_t count = 0;
    infs_status status = paged_extent_snapshot(vol, object, file, &all, &count);
    if (status != INFS_STATUS_OK)
        return status;
    uint64_t blocks = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t logical = infs_le32_to_cpu(all[i].block_count);
        uint32_t flags = infs_le32_to_cpu(all[i].flags);
        blocks += extent_physical_blocks(logical, flags);
    }
    free(all);
    *blocks_out = blocks;
    return INFS_STATUS_OK;
}
