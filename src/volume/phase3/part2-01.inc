// SPDX-License-Identifier: GPL-3.0-or-later
static int dir_validate(uint8_t block[INFS_BLOCK_SIZE],
                        struct infs_directory_payload_disk **payload_out)
{
    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
    if (!infs_validate_object_block(block) ||
        le16toh(hdr->object_type) != INFS_OBJECT_DIRECTORY ||
        le32toh(hdr->payload_size) < sizeof(struct infs_directory_payload_disk)) {
        errno = EIO;
        return -1;
    }

    struct infs_directory_payload_disk *payload =
        (struct infs_directory_payload_disk *)(block + sizeof(*hdr));
    uint32_t bytes = le32toh(payload->bytes_used);
    uint32_t count = le32toh(payload->entry_count);
    size_t available = le32toh(hdr->payload_size) - sizeof(*payload);
    if (bytes > available) {
        errno = EIO;
        return -1;
    }

    uint8_t *p = (uint8_t *)(payload + 1);
    size_t remaining = bytes;
    uint32_t seen = 0;
    while (remaining) {
        if (remaining < sizeof(struct infs_dirent_disk)) {
            errno = EIO;
            return -1;
        }
        struct infs_dirent_disk *e = (struct infs_dirent_disk *)p;
        uint16_t rec = le16toh(e->record_size);
        uint16_t name_len = le16toh(e->name_length);
        uint16_t type = le16toh(e->object_type);
        const uint8_t *name = p + sizeof(*e);
        if (rec < sizeof(*e) || rec > remaining || (rec & 7u) != 0 ||
            name_len == 0 || name_len > INFS_NAME_MAX ||
            sizeof(*e) + name_len > rec ||
            (type != INFS_OBJECT_DIRECTORY && type != INFS_OBJECT_FILE) ||
            memchr(name, '\0', name_len) != NULL || memchr(name, '/', name_len) != NULL) {
            errno = EIO;
            return -1;
        }
        p += rec;
        remaining -= rec;
        ++seen;
    }
    if (seen != count) {
        errno = EIO;
        return -1;
    }
    if (payload_out)
        *payload_out = payload;
    return 0;
}

static struct infs_dirent_disk *dir_find(struct infs_directory_payload_disk *payload,
                                          const char *name, size_t name_len,
                                          size_t *offset_out)
{
    uint8_t *p = (uint8_t *)(payload + 1);
    size_t remaining = le32toh(payload->bytes_used);
    size_t offset = 0;
    while (remaining) {
        struct infs_dirent_disk *e = (struct infs_dirent_disk *)p;
        uint16_t rec = le16toh(e->record_size);
        uint16_t n = le16toh(e->name_length);
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

static int dir_add(uint8_t block[INFS_BLOCK_SIZE], const char *name,
                   const uint8_t object_id[16], uint16_t type)
{
    struct infs_directory_payload_disk *payload;
    if (dir_validate(block, &payload) != 0)
        return -1;
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > INFS_NAME_MAX || strchr(name, '/')) {
        errno = EINVAL;
        return -1;
    }
    if (dir_find(payload, name, name_len, NULL)) {
        errno = EEXIST;
        return -1;
    }

    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
    size_t rec = align8(sizeof(struct infs_dirent_disk) + name_len);
    size_t bytes = le32toh(payload->bytes_used);
    size_t needed = sizeof(*payload) + bytes + rec;
    if (needed > INFS_BLOCK_SIZE - sizeof(*hdr)) {
        errno = ENOSPC;
        return -1;
    }

    uint8_t *p = (uint8_t *)(payload + 1) + bytes;
    memset(p, 0, rec);
    struct infs_dirent_disk *e = (struct infs_dirent_disk *)p;
    e->record_size = htole16((uint16_t)rec);
    e->name_length = htole16((uint16_t)name_len);
    e->object_type = htole16(type);
    memcpy(e->object_id, object_id, 16);
    memcpy(p + sizeof(*e), name, name_len);
    payload->entry_count = htole32(le32toh(payload->entry_count) + 1u);
    payload->bytes_used = htole32((uint32_t)(bytes + rec));
    hdr->payload_size = htole32((uint32_t)needed);
    return 0;
}
