// SPDX-License-Identifier: GPL-3.0-or-later
static int dir_remove(uint8_t block[INFS_BLOCK_SIZE], const char *name,
                      struct infs_dirent_disk *removed_copy)
{
    struct infs_directory_payload_disk *payload;
    infs_status status = dir_validate(block, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    size_t offset;
    struct infs_dirent_disk *e = dir_find(payload, name, strlen(name), &offset);
    if (!e)
        return INFS_STATUS_NOT_FOUND;
    uint16_t rec = infs_le16_to_cpu(e->record_size);
    if (removed_copy)
        memcpy(removed_copy, e, sizeof(*removed_copy));
    size_t bytes = infs_le32_to_cpu(payload->bytes_used);
    uint8_t *base = (uint8_t *)(payload + 1);
    memmove(base + offset, base + offset + rec, bytes - offset - rec);
    memset(base + bytes - rec, 0, rec);
    payload->entry_count = infs_cpu_to_le32(
        infs_le32_to_cpu(payload->entry_count) - 1u);
    payload->bytes_used = infs_cpu_to_le32((uint32_t)(bytes - rec));
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)block;
    hdr->payload_size = infs_cpu_to_le32(
        (uint32_t)(sizeof(*payload) + bytes - rec));
    return INFS_STATUS_OK;
}

static struct infs_attributes_disk *object_attributes(uint8_t block[INFS_BLOCK_SIZE])
{
    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
    uint16_t type = infs_le16_to_cpu(hdr->object_type);
    if (type == INFS_OBJECT_DIRECTORY) {
        struct infs_directory_payload_disk *p =
            (struct infs_directory_payload_disk *)(block + sizeof(*hdr));
        return &p->attributes;
    }
    if (type == INFS_OBJECT_FILE) {
        struct infs_file_payload_disk *p =
            (struct infs_file_payload_disk *)(block + sizeof(*hdr));
        return &p->attributes;
    }
    return NULL;
}

static struct infs_posix_compat_disk *object_posix(
    uint8_t block[INFS_BLOCK_SIZE])
{
    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
    uint16_t type = infs_le16_to_cpu(hdr->object_type);
    if (type == INFS_OBJECT_DIRECTORY) {
        struct infs_directory_payload_disk *p =
            (struct infs_directory_payload_disk *)(block + sizeof(*hdr));
        return &p->posix;
    }
    if (type == INFS_OBJECT_FILE) {
        struct infs_file_payload_disk *p =
            (struct infs_file_payload_disk *)(block + sizeof(*hdr));
        return &p->posix;
    }
    return NULL;
}

static void touch_mctime(struct infs_volume *vol,
                         struct infs_attributes_disk *attributes)
{
    int64_t now = 0;
    infs_status status = infs_storage_current_time_ns(&vol->storage, &now);
    if (status != INFS_STATUS_OK) {
        if (vol->tx_active && vol->tx_error == INFS_STATUS_OK)
            vol->tx_error = status;
        return;
    }
    uint64_t n = (uint64_t)now;
    attributes->modification_time_ns = (int64_t)infs_cpu_to_le64(n);
    attributes->change_time_ns = (int64_t)infs_cpu_to_le64(n);
}

static int next_component(const char **cursor, char out[INFS_NAME_MAX + 1u])
{
    const char *p = *cursor;
    while (*p == '/')
        ++p;
    if (*p == '\0') {
        *cursor = p;
        return 0;
    }
    const char *start = p;
    while (*p && *p != '/')
        ++p;
    size_t len = (size_t)(p - start);
    if (len == 0 || len > INFS_NAME_MAX)
        return INFS_STATUS_NAME_TOO_LONG;
    if (!infs_utf8_validate(start, len))
        return INFS_STATUS_INVALID_ARGUMENT;
    memcpy(out, start, len);
    out[len] = '\0';
    *cursor = p;
    return 1;
}

static int lookup_from(struct infs_volume *vol, const struct infs_lookup *start,
                       const char *relative, struct infs_lookup *out)
{
    struct infs_lookup cur = *start;
    const char *cursor = relative;
    size_t relative_len = strlen(relative);
    int trailing_slash = relative_len > 0 && relative[relative_len - 1u] == '/';
    char comp[INFS_NAME_MAX + 1u];
    int rc;
    while ((rc = next_component(&cursor, comp)) > 0) {
        /* Every component, including `.` and `..`, is directory traversal
         * through the current object. A regular file cannot be traversed. */
        if (cur.type != INFS_OBJECT_DIRECTORY)
            return INFS_STATUS_NOT_DIRECTORY;
        if (strcmp(comp, ".") == 0)
            continue;
        if (strcmp(comp, "..") == 0) {
            uint8_t block[INFS_BLOCK_SIZE];
            infs_status status = read_object(vol, cur.block, block);
            if (status != INFS_STATUS_OK)
                return status;
            struct infs_object_header_disk *hdr =
                (struct infs_object_header_disk *)block;
            if (id_is_zero(hdr->parent_id)) {
                cur.block = infs_le64_to_cpu(vol->sb.root_object_block);
                cur.type = INFS_OBJECT_DIRECTORY;
                memcpy(cur.object_id, vol->sb.root_object_id, 16);
            } else {
                status = index_find(vol, hdr->parent_id, &cur);
                if (status != INFS_STATUS_OK)
                    return status;
                if (cur.type != INFS_OBJECT_DIRECTORY)
                    return INFS_STATUS_CORRUPT;
            }
            continue;
        }
        uint8_t block[INFS_BLOCK_SIZE];
        infs_status status = read_object(vol, cur.block, block);
        if (status != INFS_STATUS_OK)
            return status;
        struct infs_directory_payload_disk *payload;
        status = dir_validate(block, &payload);
        if (status != INFS_STATUS_OK)
            return status;
        struct infs_dirent_disk *e = dir_find(payload, comp, strlen(comp), NULL);
        if (!e)
            return INFS_STATUS_NOT_FOUND;
        uint16_t type = infs_le16_to_cpu(e->object_type);
        status = index_find(vol, e->object_id, &cur);
        if (status != INFS_STATUS_OK)
            return status;
        if (cur.type != type)
            return INFS_STATUS_CORRUPT;
    }
    if (rc < 0)
        return rc;
    if (trailing_slash && cur.type != INFS_OBJECT_DIRECTORY)
        return INFS_STATUS_NOT_DIRECTORY;
    *out = cur;
    return INFS_STATUS_OK;
}
