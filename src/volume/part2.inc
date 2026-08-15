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
    uint16_t rec = le16toh(e->record_size);
    if (removed_copy)
        memcpy(removed_copy, e, sizeof(*removed_copy));
    size_t bytes = le32toh(payload->bytes_used);
    uint8_t *base = (uint8_t *)(payload + 1);
    memmove(base + offset, base + offset + rec, bytes - offset - rec);
    memset(base + bytes - rec, 0, rec);
    payload->entry_count = htole32(le32toh(payload->entry_count) - 1u);
    payload->bytes_used = htole32((uint32_t)(bytes - rec));
    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
    hdr->payload_size = htole32((uint32_t)(sizeof(*payload) + bytes - rec));
    return 0;
}

static struct infs_stat_disk *object_stat(uint8_t block[INFS_BLOCK_SIZE])
{
    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
    uint16_t type = le16toh(hdr->object_type);
    if (type == INFS_OBJECT_DIRECTORY) {
        struct infs_directory_payload_disk *p =
            (struct infs_directory_payload_disk *)(block + sizeof(*hdr));
        return &p->stat;
    }
    if (type == INFS_OBJECT_FILE) {
        struct infs_file_payload_disk *p =
            (struct infs_file_payload_disk *)(block + sizeof(*hdr));
        return &p->stat;
    }
    return NULL;
}

static void touch_mctime(struct infs_stat_disk *st)
{
    uint64_t n = now_ns();
    st->mtime_ns = htole64(n);
    st->ctime_ns = htole64(n);
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
    if (len == 0 || len > INFS_NAME_MAX) {
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
                cur.block = le64toh(vol->sb.root_object_block);
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
        uint16_t type = le16toh(e->object_type);
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

int infs_lookup_path(struct infs_volume *vol, const char *path,
                     struct infs_lookup *out)
{
    if (!vol || !path || !out || path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    struct infs_lookup root = {
        .block = le64toh(vol->sb.root_object_block),
        .type = INFS_OBJECT_DIRECTORY
    };
    memcpy(root.object_id, vol->sb.root_object_id, 16);
    return lookup_from(vol, &root, path, out);
}

static int split_parent(struct infs_volume *vol, const char *path,
                        struct infs_lookup *parent, char name[INFS_NAME_MAX + 1u])
{
    if (!path || path[0] != '/' || strcmp(path, "/") == 0) {
        errno = EINVAL;
        return -1;
    }
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        --len;
    const char *slash = path + len;
    while (slash > path && slash[-1] != '/')
        --slash;
    size_t name_len = (size_t)((path + len) - slash);
    if (name_len == 0 || name_len > INFS_NAME_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(name, slash, name_len);
    name[name_len] = '\0';
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        errno = EINVAL;
        return -1;
    }

    char parent_path[PATH_MAX];
    size_t plen = (size_t)(slash - path);
    if (plen == 0)
        plen = 1;
    if (plen >= sizeof(parent_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(parent_path, path, plen);
    parent_path[plen] = '\0';
    if (infs_lookup_path(vol, parent_path, parent) != 0)
        return -1;
    if (parent->type != INFS_OBJECT_DIRECTORY) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

static int create_object(struct infs_volume *vol, const char *path,
                         mode_t mode, uid_t uid, gid_t gid, uint16_t type)
{
    if (!vol->writable) {
        errno = EROFS;
        return -1;
    }
    struct infs_lookup parent;
    char name[INFS_NAME_MAX + 1u];
    if (split_parent(vol, path, &parent, name) != 0)
        return -1;

    uint8_t parent_block[INFS_BLOCK_SIZE];
    if (read_object(vol, parent.block, parent_block) != 0)
        return -1;
    struct infs_directory_payload_disk *parent_payload;
    if (dir_validate(parent_block, &parent_payload) != 0)
        return -1;
    if (dir_find(parent_payload, name, strlen(name), NULL)) {
        errno = EEXIST;
        return -1;
    }

    uint64_t object_block, got;
    if (alloc_run(vol, 1, &object_block, &got) != 0)
        return -1;
    if (got != 1) {
        errno = EIO;
        return -1;
    }

    uint8_t object_id[16];
    if (infs_random_bytes(object_id, sizeof(object_id)) != 0) {
        free_run(vol, object_block, 1);
        return -1;
    }

    uint8_t object[INFS_BLOCK_SIZE];
    uint64_t n = now_ns();
    uint32_t payload_size;
    if (type == INFS_OBJECT_DIRECTORY)
        payload_size = sizeof(struct infs_directory_payload_disk);
    else
        payload_size = sizeof(struct infs_file_payload_disk);
    if (infs_object_init(object, type, object_id, parent.object_id,
                         le64toh(vol->sb.generation) + 1u, payload_size) != 0) {
        free_run(vol, object_block, 1);
        return -1;
    }

    struct infs_stat_disk *st;
    if (type == INFS_OBJECT_DIRECTORY) {
        struct infs_directory_payload_disk *p =
            (struct infs_directory_payload_disk *)(object + sizeof(struct infs_object_header_disk));
        memset(p, 0, sizeof(*p));
        st = &p->stat;
        st->mode = htole32((uint32_t)(S_IFDIR | (mode & 07777u)));
        st->nlink = htole32(2);
    } else {
        struct infs_file_payload_disk *p =
            (struct infs_file_payload_disk *)(object + sizeof(struct infs_object_header_disk));
        memset(p, 0, sizeof(*p));
        st = &p->stat;
        st->mode = htole32((uint32_t)(S_IFREG | (mode & 07777u)));
        st->nlink = htole32(1);
    }
    st->uid = htole32((uint32_t)uid);
    st->gid = htole32((uint32_t)gid);
    st->size = htole64(0);
    st->atime_ns = htole64(n);
    st->mtime_ns = htole64(n);
    st->ctime_ns = htole64(n);

    if (infs_object_finalize(object) != 0 || write_block(vol, object_block, object) != 0) {
        free_run(vol, object_block, 1);
        return -1;
    }
    if (index_add(vol, object_id, object_block, type) != 0) {
        free_run(vol, object_block, 1);
        return -1;
    }
    if (dir_add(parent_block, name, object_id, type) != 0) {
        (void)index_remove(vol, object_id);
        free_run(vol, object_block, 1);
        return -1;
    }
    if (type == INFS_OBJECT_DIRECTORY)
        parent_payload->stat.nlink = htole32(le32toh(parent_payload->stat.nlink) + 1u);
    touch_mctime(&parent_payload->stat);
    if (write_object(vol, parent.block, parent_block) != 0)
        return -1;
    return infs_volume_sync(vol);
}
