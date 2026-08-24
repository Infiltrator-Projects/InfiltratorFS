// SPDX-License-Identifier: GPL-3.0-or-later
static int dir_validate(uint8_t block[INFS_BLOCK_SIZE],
                        struct infs_directory_payload_disk **payload_out)
{
    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
    if (!infs_validate_object_block(block) ||
        infs_le16_to_cpu(hdr->object_type) != INFS_OBJECT_DIRECTORY ||
        infs_le32_to_cpu(hdr->payload_size) < sizeof(struct infs_directory_payload_disk)) {
        return INFS_STATUS_CORRUPT;
    }

    struct infs_directory_payload_disk *payload =
        (struct infs_directory_payload_disk *)(block + sizeof(*hdr));
    if (validate_common_metadata(&payload->attributes, &payload->posix) !=
        INFS_STATUS_OK)
        return INFS_STATUS_CORRUPT;

    uint32_t count = infs_le32_to_cpu(payload->entry_count);
    uint16_t version = infs_le16_to_cpu(hdr->object_version);
    if (version == INFS_OBJECT_VERSION_PAGED) {
        uint32_t page_count = infs_le32_to_cpu(payload->bytes_used);
        size_t expected = sizeof(*payload) +
            (size_t)page_count * sizeof(uint64_t);
        if (page_count > INFS_DIRECTORY_PAGE_POINTERS ||
            expected != infs_le32_to_cpu(hdr->payload_size) ||
            (count == 0 && page_count != 0) ||
            (count != 0 && page_count == 0))
            return INFS_STATUS_CORRUPT;
        if (payload_out)
            *payload_out = payload;
        return INFS_STATUS_OK;
    }

    uint32_t bytes = infs_le32_to_cpu(payload->bytes_used);
    size_t available = infs_le32_to_cpu(hdr->payload_size) - sizeof(*payload);
    if (bytes != available)
        return INFS_STATUS_CORRUPT;

    uint8_t *p = (uint8_t *)(payload + 1);
    size_t remaining = bytes;
    uint32_t seen = 0;
    while (remaining) {
        if (remaining < sizeof(struct infs_dirent_disk))
            return INFS_STATUS_CORRUPT;
        struct infs_dirent_disk *e = (struct infs_dirent_disk *)p;
        uint16_t rec = infs_le16_to_cpu(e->record_size);
        uint16_t name_len = infs_le16_to_cpu(e->name_length);
        uint16_t type = infs_le16_to_cpu(e->object_type);
        const uint8_t *name = p + sizeof(*e);
        if (rec < sizeof(*e) || rec > remaining || (rec & 7u) != 0 ||
            name_len == 0 || name_len > INFS_NAME_MAX ||
            sizeof(*e) + name_len > rec ||
            !namespace_object_type(type) ||
            infs_le16_to_cpu(e->flags) != 0 || id_is_zero(e->object_id) ||
            memchr(name, '\0', name_len) != NULL || memchr(name, '/', name_len) != NULL)
            return INFS_STATUS_CORRUPT;
        if (!infs_utf8_validate(name, name_len))
            return INFS_STATUS_CORRUPT;
        if ((name_len == 1u && name[0] == '.') ||
            (name_len == 2u && name[0] == '.' && name[1] == '.'))
            return INFS_STATUS_CORRUPT;
        for (size_t i = sizeof(*e) + name_len; i < rec; ++i)
            if (p[i] != 0)
                return INFS_STATUS_CORRUPT;
        p += rec;
        remaining -= rec;
        ++seen;
    }
    if (seen != count)
        return INFS_STATUS_CORRUPT;
    if (payload_out)
        *payload_out = payload;
    return INFS_STATUS_OK;
}

static int symlink_validate(
    struct infs_volume *vol, uint8_t block[INFS_BLOCK_SIZE],
    struct infs_symlink_payload_disk **payload_out,
    const uint8_t **target_out)
{
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)block;
    if (!symbolic_links_enabled(vol) || !infs_validate_object_block(block) ||
        infs_le16_to_cpu(header->object_type) != INFS_OBJECT_SYMLINK ||
        infs_le16_to_cpu(header->object_version) !=
            INFS_OBJECT_VERSION_CLASSIC)
        return INFS_STATUS_CORRUPT;
    struct infs_symlink_payload_disk *payload =
        (struct infs_symlink_payload_disk *)(header + 1);
    if (validate_common_metadata(&payload->attributes, &payload->posix) !=
            INFS_STATUS_OK ||
        infs_le64_to_cpu(payload->attributes.link_count) != 1u)
        return INFS_STATUS_CORRUPT;
    if (payload_out)
        *payload_out = payload;
    if (target_out)
        *target_out = (const uint8_t *)(payload + 1);
    return INFS_STATUS_OK;
}

static struct infs_dirent_disk *dir_find(struct infs_directory_payload_disk *payload,
                                          const char *name, size_t name_len,
                                          size_t *offset_out)
{
    uint8_t *p = (uint8_t *)(payload + 1);
    size_t remaining = infs_le32_to_cpu(payload->bytes_used);
    size_t offset = 0;
    while (remaining) {
        struct infs_dirent_disk *e = (struct infs_dirent_disk *)p;
        uint16_t rec = infs_le16_to_cpu(e->record_size);
        uint16_t n = infs_le16_to_cpu(e->name_length);
        if (n == name_len && memcmp(p + sizeof(*e), name, name_len) == 0) {
            if (offset_out)
                *offset_out = offset;
            return e;
        }
        p += rec;
        remaining -= rec;
        offset += rec;
    }
    return NULL;
}

static int dir_lookup_volume(struct infs_volume *vol,
                             uint8_t block[INFS_BLOCK_SIZE], const char *name,
                             struct infs_dirent_disk *entry_out)
{
    struct infs_directory_payload_disk *payload;
    infs_status status = dir_validate(block, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)block;
    if (infs_le16_to_cpu(header->object_version) ==
        INFS_OBJECT_VERSION_PAGED)
        return paged_dir_lookup(vol, block, name, entry_out);

    struct infs_dirent_disk *entry =
        dir_find(payload, name, strlen(name), NULL);
    if (!entry)
        return INFS_STATUS_NOT_FOUND;
    if (entry_out)
        memcpy(entry_out, entry, sizeof(*entry_out));
    return INFS_STATUS_OK;
}

static int dir_add(uint8_t block[INFS_BLOCK_SIZE], const char *name,
                   const uint8_t object_id[16], uint16_t type)
{
    struct infs_directory_payload_disk *payload;
    int status = dir_validate(block, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > INFS_NAME_MAX || strchr(name, '/') ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
        id_is_zero(object_id) || !infs_utf8_validate(name, name_len) ||
        !namespace_object_type(type))
        return INFS_STATUS_INVALID_ARGUMENT;
    if (dir_find(payload, name, name_len, NULL))
        return INFS_STATUS_ALREADY_EXISTS;

    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
    size_t rec = align8(sizeof(struct infs_dirent_disk) + name_len);
    size_t bytes = infs_le32_to_cpu(payload->bytes_used);
    size_t needed = sizeof(*payload) + bytes + rec;
    if (needed > INFS_BLOCK_SIZE - sizeof(*hdr))
        return INFS_STATUS_NO_SPACE;

    uint8_t *p = (uint8_t *)(payload + 1) + bytes;
    memset(p, 0, rec);
    struct infs_dirent_disk *e = (struct infs_dirent_disk *)p;
    e->record_size = infs_cpu_to_le16((uint16_t)rec);
    e->name_length = infs_cpu_to_le16((uint16_t)name_len);
    e->object_type = infs_cpu_to_le16(type);
    memcpy(e->object_id, object_id, 16);
    memcpy(p + sizeof(*e), name, name_len);
    payload->entry_count = infs_cpu_to_le32(infs_le32_to_cpu(payload->entry_count) + 1u);
    payload->bytes_used = infs_cpu_to_le32((uint32_t)(bytes + rec));
    hdr->payload_size = infs_cpu_to_le32((uint32_t)needed);
    return INFS_STATUS_OK;
}

static int dir_add_volume(struct infs_volume *vol,
                          uint8_t block[INFS_BLOCK_SIZE], const char *name,
                          const uint8_t object_id[16], uint16_t type)
{
    struct infs_directory_payload_disk *payload;
    infs_status status = dir_validate(block, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    (void)payload;
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)block;
    if (infs_le16_to_cpu(header->object_version) ==
        INFS_OBJECT_VERSION_PAGED)
        return paged_dir_add(vol, block, name, object_id, type);
    return dir_add(block, name, object_id, type);
}
