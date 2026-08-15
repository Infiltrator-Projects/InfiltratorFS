// SPDX-License-Identifier: GPL-3.0-or-later
static int dir_remove(uint8_t block[INFS_BLOCK_SIZE], const char *name,
                      struct infs_dirent_disk *removed_copy)
{
    struct infs_directory_payload_disk *payload;
    if (dir_validate(block, &payload) != 0)
        return -1;
    size_t offset;
    struct infs_dirent_disk *e = dir_find(payload, name, strlen(name), &offset);
    if (!e) {
        errno = ENOENT;
        return -1;
    }
    uint16_t rec = infs_le16_to_cpu(e->record_size);
    if (removed_copy)
        memcpy(removed_copy, e, sizeof(*removed_copy));
    size_t bytes = infs_le32_to_cpu(payload->bytes_used);
    uint8_t *base = (uint8_t *)(payload + 1);
    memmove(base + offset, base + offset + rec, bytes - offset - rec);
    memset(base + bytes - rec, 0, rec);
    payload->entry_count = infs_cpu_to_le32(infs_le32_to_cpu(payload->entry_count) - 1u);
    payload->bytes_used = infs_cpu_to_le32((uint32_t)(bytes - rec));
    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
    hdr->payload_size = infs_cpu_to_le32((uint32_t)(sizeof(*payload) + bytes - rec));
    return 0;
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
    uint64_t n = (uint64_t)now_ns(vol);
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
    if (len == 0 || len > INFS_NAME_MAX || !infs_utf8_validate(start, len)) {
        errno = ENAMETOOLONG;
        return -1;
    }
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
    char comp[INFS_NAME_MAX + 1u];
    int rc;
    while ((rc = next_component(&cursor, comp)) > 0) {
        if (strcmp(comp, ".") == 0)
            continue;
        if (strcmp(comp, "..") == 0) {
            uint8_t block[INFS_BLOCK_SIZE];
            if (read_object(vol, cur.block, block) != 0)
                return -1;
            struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
            if (id_is_zero(hdr->parent_id)) {
                cur.block = infs_le64_to_cpu(vol->sb.root_object_block);
                cur.type = INFS_OBJECT_DIRECTORY;
                memcpy(cur.object_id, vol->sb.root_object_id, 16);
            } else if (index_find(vol, hdr->parent_id, &cur) != 0) {
                return -1;
            }
            continue;
        }
        if (cur.type != INFS_OBJECT_DIRECTORY) {
            errno = ENOTDIR;
            return -1;
        }
        uint8_t block[INFS_BLOCK_SIZE];
        if (read_object(vol, cur.block, block) != 0)
            return -1;
        struct infs_directory_payload_disk *payload;
        if (dir_validate(block, &payload) != 0)
            return -1;
        struct infs_dirent_disk *e = dir_find(payload, comp, strlen(comp), NULL);
        if (!e) {
            errno = ENOENT;
            return -1;
        }
        uint16_t type = infs_le16_to_cpu(e->object_type);
        if (index_find(vol, e->object_id, &cur) != 0)
            return -1;
        if (cur.type != type) {
            errno = EIO;
            return -1;
        }
    }
    if (rc < 0)
        return -1;
    *out = cur;
    return 0;
}
