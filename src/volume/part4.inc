// SPDX-License-Identifier: GPL-3.0-or-later
int infs_getattr(struct infs_volume *vol, const char *path, struct stat *st)
{
    if (!st) {
        errno = EINVAL;
        return -1;
    }
    struct infs_lookup lk;
    if (infs_lookup_path(vol, path, &lk) != 0)
        return -1;
    uint8_t block[INFS_BLOCK_SIZE];
    if (read_object(vol, lk.block, block) != 0)
        return -1;
    struct infs_stat_disk *ds = object_stat(block);
    if (!ds) {
        errno = EIO;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = le32toh(ds->mode);
    st->st_uid = le32toh(ds->uid);
    st->st_gid = le32toh(ds->gid);
    st->st_nlink = le32toh(ds->nlink);
    st->st_size = (off_t)le64toh(ds->size);
    st->st_blksize = INFS_BLOCK_SIZE;
    st->st_ino = (ino_t)lk.block;
    uint64_t at = le64toh(ds->atime_ns);
    uint64_t mt = le64toh(ds->mtime_ns);
    uint64_t ct = le64toh(ds->ctime_ns);
    st->st_atim.tv_sec = (time_t)(at / 1000000000ull);
    st->st_atim.tv_nsec = (long)(at % 1000000000ull);
    st->st_mtim.tv_sec = (time_t)(mt / 1000000000ull);
    st->st_mtim.tv_nsec = (long)(mt % 1000000000ull);
    st->st_ctim.tv_sec = (time_t)(ct / 1000000000ull);
    st->st_ctim.tv_nsec = (long)(ct % 1000000000ull);

    if (S_ISREG(st->st_mode)) {
        struct infs_file_payload_disk *p;
        struct infs_extent_disk *ext;
        if (file_validate_volume(vol, block, &p, &ext) != 0)
            return -1;
        st->st_blocks = (blkcnt_t)(file_allocated_blocks(p, ext) * (INFS_BLOCK_SIZE / 512u));
    } else {
        st->st_blocks = INFS_BLOCK_SIZE / 512u;
    }
    return 0;
}

int infs_list_dir(struct infs_volume *vol, const char *path,
                  struct infs_dir_item **items, size_t *count_out)
{
    if (!items || !count_out) {
        errno = EINVAL;
        return -1;
    }
    *items = NULL;
    *count_out = 0;
    struct infs_lookup lk;
    if (infs_lookup_path(vol, path, &lk) != 0)
        return -1;
    if (lk.type != INFS_OBJECT_DIRECTORY) {
        errno = ENOTDIR;
        return -1;
    }
    uint8_t block[INFS_BLOCK_SIZE];
    if (read_object(vol, lk.block, block) != 0)
        return -1;
    struct infs_directory_payload_disk *payload;
    if (dir_validate(block, &payload) != 0)
        return -1;
    uint32_t count = le32toh(payload->entry_count);
    struct infs_dir_item *list = calloc(count ? count : 1u, sizeof(*list));
    if (!list)
        return -1;
    uint8_t *p = (uint8_t *)(payload + 1);
    for (uint32_t i = 0; i < count; ++i) {
        struct infs_dirent_disk *e = (struct infs_dirent_disk *)p;
        uint16_t n = le16toh(e->name_length);
        memcpy(list[i].name, p + sizeof(*e), n);
        list[i].name[n] = '\0';
        memcpy(list[i].object_id, e->object_id, 16);
        list[i].type = le16toh(e->object_type);
        p += le16toh(e->record_size);
    }
    *items = list;
    *count_out = count;
    return 0;
}

void infs_free_dir_items(struct infs_dir_item *items)
{
    free(items);
}

int infs_create_file(struct infs_volume *vol, const char *path, mode_t mode,
                     uid_t uid, gid_t gid)
{
    return create_object(vol, path, mode, uid, gid, INFS_OBJECT_FILE);
}

int infs_mkdir(struct infs_volume *vol, const char *path, mode_t mode,
               uid_t uid, gid_t gid)
{
    return create_object(vol, path, mode, uid, gid, INFS_OBJECT_DIRECTORY);
}

ssize_t infs_read_file(struct infs_volume *vol, const char *path, void *buf,
                       size_t size, off_t offset)
{
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    struct infs_lookup lk;
    if (infs_lookup_path(vol, path, &lk) != 0)
        return -1;
    if (lk.type != INFS_OBJECT_FILE) {
        errno = EISDIR;
        return -1;
    }
    uint8_t object[INFS_BLOCK_SIZE];
    if (read_object(vol, lk.block, object) != 0)
        return -1;
    struct infs_file_payload_disk *p;
    struct infs_extent_disk *ext;
    if (file_validate_volume(vol, object, &p, &ext) != 0)
        return -1;
    uint64_t file_size = le64toh(p->stat.size);
    uint64_t off = (uint64_t)offset;
    if (off >= file_size)
        return 0;
    uint64_t remaining64 = file_size - off;
    size_t remaining = size;
    if ((uint64_t)remaining > remaining64)
        remaining = (size_t)remaining64;
    size_t done = 0;
    uint8_t data[INFS_BLOCK_SIZE];
    while (done < remaining) {
        uint64_t pos = off + done;
        uint64_t logical = pos / INFS_BLOCK_SIZE;
        size_t within = (size_t)(pos % INFS_BLOCK_SIZE);
        size_t chunk = INFS_BLOCK_SIZE - within;
        if (chunk > remaining - done)
            chunk = remaining - done;
        uint64_t physical;
        if (file_map_block(p, ext, logical, &physical) != 0 ||
            read_block(vol, physical, data) != 0)
            return -1;
        memcpy((uint8_t *)buf + done, data + within, chunk);
        done += chunk;
    }
    return (ssize_t)done;
}

ssize_t infs_write_file(struct infs_volume *vol, const char *path,
                        const void *buf, size_t size, off_t offset)
{
    if (!vol->writable) {
        errno = EROFS;
        return -1;
    }
    if (offset < 0 || (uint64_t)offset > UINT64_MAX - size) {
        errno = EINVAL;
        return -1;
    }
    if (size == 0)
        return 0;
    struct infs_lookup lk;
    if (infs_lookup_path(vol, path, &lk) != 0)
        return -1;
    if (lk.type != INFS_OBJECT_FILE) {
        errno = EISDIR;
        return -1;
    }
    uint8_t object[INFS_BLOCK_SIZE];
    if (read_object(vol, lk.block, object) != 0)
        return -1;
    struct infs_file_payload_disk *p;
    struct infs_extent_disk *ext;
    if (file_validate_volume(vol, object, &p, &ext) != 0)
        return -1;
    uint64_t end = (uint64_t)offset + size;
    uint64_t needed_blocks = (end + INFS_BLOCK_SIZE - 1u) / INFS_BLOCK_SIZE;
    if (file_ensure_blocks(vol, object, needed_blocks) != 0)
        return -1;
    if (infs_object_finalize(object) != 0 ||
        file_validate_volume(vol, object, &p, &ext) != 0)
        return -1;

    size_t done = 0;
    uint8_t data[INFS_BLOCK_SIZE];
    while (done < size) {
        uint64_t pos = (uint64_t)offset + done;
        uint64_t logical = pos / INFS_BLOCK_SIZE;
        size_t within = (size_t)(pos % INFS_BLOCK_SIZE);
        size_t chunk = INFS_BLOCK_SIZE - within;
        if (chunk > size - done)
            chunk = size - done;
        uint64_t physical;
        if (file_map_block(p, ext, logical, &physical) != 0)
            return -1;
        if (within != 0 || chunk != INFS_BLOCK_SIZE) {
            if (read_block(vol, physical, data) != 0)
                return -1;
        } else {
            memset(data, 0, sizeof(data));
        }
        memcpy(data + within, (const uint8_t *)buf + done, chunk);
        if (write_block(vol, physical, data) != 0)
            return -1;
        done += chunk;
    }
    if (end > le64toh(p->stat.size))
        p->stat.size = htole64(end);
    touch_mctime(&p->stat);
    if (write_object(vol, lk.block, object) != 0 || infs_volume_sync(vol) != 0)
        return -1;
    return (ssize_t)done;
}

int infs_truncate_file(struct infs_volume *vol, const char *path, off_t size)
{
    if (!vol->writable) {
        errno = EROFS;
        return -1;
    }
    if (size < 0) {
        errno = EINVAL;
        return -1;
    }
    struct infs_lookup lk;
    if (infs_lookup_path(vol, path, &lk) != 0)
        return -1;
    if (lk.type != INFS_OBJECT_FILE) {
        errno = EISDIR;
        return -1;
    }
    uint8_t object[INFS_BLOCK_SIZE];
    if (read_object(vol, lk.block, object) != 0)
        return -1;
    struct infs_file_payload_disk *p;
    struct infs_extent_disk *ext;
    if (file_validate_volume(vol, object, &p, &ext) != 0)
        return -1;

    uint64_t old_size = le64toh(p->stat.size);
    uint64_t target_blocks = ((uint64_t)size + INFS_BLOCK_SIZE - 1u) / INFS_BLOCK_SIZE;
    uint64_t have = file_allocated_blocks(p, ext);
    if (target_blocks > have) {
        if (file_ensure_blocks(vol, object, target_blocks) != 0)
            return -1;
        if (infs_object_finalize(object) != 0 ||
            file_validate_volume(vol, object, &p, &ext) != 0)
            return -1;
    } else if (target_blocks < have) {
        uint32_t count = le32toh(p->extent_count);
        uint32_t keep = 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t logical = le64toh(ext[i].logical_block);
            uint64_t physical = le64toh(ext[i].physical_block);
            uint32_t blocks = le32toh(ext[i].block_count);
            if (logical >= target_blocks) {
                free_run(vol, physical, blocks);
                continue;
            }
            uint64_t end = logical + blocks;
            if (end > target_blocks) {
                uint32_t retained = (uint32_t)(target_blocks - logical);
                free_run(vol, physical + retained, blocks - retained);
                ext[i].block_count = htole32(retained);
            }
            if (keep != i)
                ext[keep] = ext[i];
            ++keep;
        }
        if (keep < count)
            memset(&ext[keep], 0, (size_t)(count - keep) * sizeof(*ext));
        p->extent_count = htole32(keep);
        struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)object;
        hdr->payload_size = htole32((uint32_t)(sizeof(*p) +
            (size_t)keep * sizeof(*ext)));
    }

    if ((uint64_t)size < old_size && target_blocks > 0 && ((uint64_t)size % INFS_BLOCK_SIZE) != 0) {
        if (infs_object_finalize(object) != 0 ||
            file_validate_volume(vol, object, &p, &ext) != 0)
            return -1;
        uint64_t logical = target_blocks - 1u;
        uint64_t physical;
        if (file_map_block(p, ext, logical, &physical) != 0)
            return -1;
        uint8_t data[INFS_BLOCK_SIZE];
        if (read_block(vol, physical, data) != 0)
            return -1;
        size_t tail = (size_t)((uint64_t)size % INFS_BLOCK_SIZE);
        memset(data + tail, 0, INFS_BLOCK_SIZE - tail);
        if (write_block(vol, physical, data) != 0)
            return -1;
    }

    p->stat.size = htole64((uint64_t)size);
    touch_mctime(&p->stat);
    if (write_object(vol, lk.block, object) != 0)
        return -1;
    return infs_volume_sync(vol);
}
