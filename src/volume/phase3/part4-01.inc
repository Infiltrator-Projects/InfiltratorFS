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
    st->st_ino = object_ino(lk.object_id);
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
    if (tx_begin(vol) != 0)
        return -1;
    int rc = create_object(vol, path, mode, uid, gid, INFS_OBJECT_FILE);
    if (rc != 0) {
        int saved = errno;
        tx_abort(vol);
        errno = saved;
    }
    return rc;
}

int infs_mkdir(struct infs_volume *vol, const char *path, mode_t mode,
               uid_t uid, gid_t gid)
{
    if (tx_begin(vol) != 0)
        return -1;
    int rc = create_object(vol, path, mode, uid, gid, INFS_OBJECT_DIRECTORY);
    if (rc != 0) {
        int saved = errno;
        tx_abort(vol);
        errno = saved;
    }
    return rc;
}
