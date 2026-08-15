// SPDX-License-Identifier: GPL-3.0-or-later
static int rename_tx(struct infs_volume *vol, const char *oldpath, const char *newpath)
{
    if (!vol->writable) {
        errno = EROFS;
        return -1;
    }
    struct infs_lookup old_parent, new_parent;
    char old_name[INFS_NAME_MAX + 1u], new_name[INFS_NAME_MAX + 1u];
    if (split_parent(vol, oldpath, &old_parent, old_name) != 0 ||
        split_parent(vol, newpath, &new_parent, new_name) != 0)
        return -1;

    uint8_t old_block[INFS_BLOCK_SIZE], new_block[INFS_BLOCK_SIZE];
    if (read_object(vol, old_parent.block, old_block) != 0)
        return -1;
    struct infs_directory_payload_disk *op;
    if (dir_validate(old_block, &op) != 0)
        return -1;
    struct infs_dirent_disk *source = dir_find(op, old_name, strlen(old_name), NULL);
    if (!source) {
        errno = ENOENT;
        return -1;
    }
    uint8_t source_id[16];
    memcpy(source_id, source->object_id, 16);
    uint16_t source_type = le16toh(source->object_type);

    if (old_parent.block == new_parent.block) {
        memcpy(new_block, old_block, sizeof(new_block));
    } else if (read_object(vol, new_parent.block, new_block) != 0) {
        return -1;
    }
    struct infs_directory_payload_disk *np;
    if (dir_validate(new_block, &np) != 0)
        return -1;
    if (dir_find(np, new_name, strlen(new_name), NULL)) {
        errno = EEXIST;
        return -1;
    }

    if (source_type == INFS_OBJECT_DIRECTORY) {
        int a = id_is_ancestor(vol, source_id, new_parent.object_id);
        if (a < 0)
            return -1;
        if (a) {
            errno = EINVAL;
            return -1;
        }
    }

    if (old_parent.block == new_parent.block) {
        if (dir_remove(new_block, old_name, NULL) != 0 ||
            infs_object_finalize(new_block) != 0 ||
            dir_add(new_block, new_name, source_id, source_type) != 0)
            return -1;
        np = (struct infs_directory_payload_disk *)(new_block + sizeof(struct infs_object_header_disk));
        touch_mctime(&np->stat);
        if (write_object(vol, new_parent.block, new_block) != 0)
            return -1;
    } else {
        if (dir_add(new_block, new_name, source_id, source_type) != 0 ||
            dir_remove(old_block, old_name, NULL) != 0)
            return -1;
        op = (struct infs_directory_payload_disk *)(old_block + sizeof(struct infs_object_header_disk));
        np = (struct infs_directory_payload_disk *)(new_block + sizeof(struct infs_object_header_disk));
        if (source_type == INFS_OBJECT_DIRECTORY) {
            uint32_t on = le32toh(op->stat.nlink);
            if (on > 2)
                op->stat.nlink = htole32(on - 1u);
            np->stat.nlink = htole32(le32toh(np->stat.nlink) + 1u);
        }
        touch_mctime(&op->stat);
        touch_mctime(&np->stat);
        if (write_object(vol, old_parent.block, old_block) != 0 ||
            write_object(vol, new_parent.block, new_block) != 0)
            return -1;

        struct infs_lookup child;
        if (index_find(vol, source_id, &child) != 0)
            return -1;
        uint8_t child_block[INFS_BLOCK_SIZE];
        if (read_object(vol, child.block, child_block) != 0)
            return -1;
        struct infs_object_header_disk *ch = (struct infs_object_header_disk *)child_block;
        memcpy(ch->parent_id, new_parent.object_id, 16);
        struct infs_stat_disk *st = object_stat(child_block);
        if (st)
            st->ctime_ns = htole64(now_ns());
        if (write_object(vol, child.block, child_block) != 0)
            return -1;
    }
    return infs_volume_sync(vol);
}

int infs_rename(struct infs_volume *vol, const char *oldpath, const char *newpath)
{
    if (tx_begin(vol) != 0)
        return -1;
    int rc = rename_tx(vol, oldpath, newpath);
    if (rc != 0) {
        int saved = errno;
        tx_abort(vol);
        errno = saved;
    }
    return rc;
}
