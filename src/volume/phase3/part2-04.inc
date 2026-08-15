// SPDX-License-Identifier: GPL-3.0-or-later
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
        p->data_checksum_type = htole32(INFS_CHECKSUM_SHA256);
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
