// SPDX-License-Identifier: GPL-3.0-or-later
static int remove_object(struct infs_volume *vol, const char *path, int want_directory)
{
    struct infs_lookup parent;
    char name[INFS_NAME_MAX + 1u];
    if (split_parent(vol, path, &parent, name) != 0)
        return -1;
    uint8_t parent_block[INFS_BLOCK_SIZE];
    if (read_object(vol, parent.block, parent_block) != 0)
        return -1;
    struct infs_directory_payload_disk *pp;
    if (dir_validate(parent_block, &pp) != 0)
        return -1;
    struct infs_dirent_disk *de = dir_find(pp, name, strlen(name), NULL);
    if (!de) {
        errno = ENOENT;
        return -1;
    }
    struct infs_lookup child;
    if (index_find(vol, de->object_id, &child) != 0)
        return -1;
    if (want_directory && child.type != INFS_OBJECT_DIRECTORY) {
        errno = ENOTDIR;
        return -1;
    }
    if (!want_directory && child.type == INFS_OBJECT_DIRECTORY) {
        errno = EISDIR;
        return -1;
    }

    uint8_t child_block[INFS_BLOCK_SIZE];
    if (read_object(vol, child.block, child_block) != 0)
        return -1;
    if (child.type == INFS_OBJECT_DIRECTORY) {
        struct infs_directory_payload_disk *cp;
        if (dir_validate(child_block, &cp) != 0)
            return -1;
        if (le32toh(cp->entry_count) != 0) {
            errno = ENOTEMPTY;
            return -1;
        }
    } else if (child.type == INFS_OBJECT_FILE) {
        struct infs_file_payload_disk *fp;
        struct infs_extent_disk *ext;
        if (file_validate_volume(vol, child_block, &fp, &ext) != 0)
            return -1;
        if (checksum_destroy_chain(vol, fp, child.object_id) != 0)
            return -1;
        uint32_t count = le32toh(fp->extent_count);
        for (uint32_t i = 0; i < count; ++i)
            free_run(vol, le64toh(ext[i].physical_block), le32toh(ext[i].block_count));
    }

    if (dir_remove(parent_block, name, NULL) != 0)
        return -1;
    if (child.type == INFS_OBJECT_DIRECTORY) {
        uint32_t nlink = le32toh(pp->stat.nlink);
        if (nlink > 2)
            pp->stat.nlink = htole32(nlink - 1u);
    }
    touch_mctime(&pp->stat);
    if (write_object(vol, parent.block, parent_block) != 0)
        return -1;
    if (index_remove(vol, child.object_id) != 0)
        return -1;
    free_run(vol, child.block, 1);
    return infs_volume_sync(vol);
}

int infs_unlink(struct infs_volume *vol, const char *path)
{
    if (tx_begin(vol) != 0)
        return -1;
    int rc = remove_object(vol, path, 0);
    if (rc != 0) {
        int saved = errno;
        tx_abort(vol);
        errno = saved;
    }
    return rc;
}

int infs_rmdir(struct infs_volume *vol, const char *path)
{
    if (tx_begin(vol) != 0)
        return -1;
    int rc = remove_object(vol, path, 1);
    if (rc != 0) {
        int saved = errno;
        tx_abort(vol);
        errno = saved;
    }
    return rc;
}

static int id_is_ancestor(struct infs_volume *vol, const uint8_t ancestor[16],
                          const uint8_t start[16])
{
    uint8_t cur[16];
    memcpy(cur, start, 16);
    for (unsigned depth = 0; depth < 1024; ++depth) {
        if (memcmp(cur, ancestor, 16) == 0)
            return 1;
        if (memcmp(cur, vol->sb.root_object_id, 16) == 0)
            return 0;
        struct infs_lookup lk;
        if (index_find(vol, cur, &lk) != 0)
            return -1;
        uint8_t block[INFS_BLOCK_SIZE];
        if (read_object(vol, lk.block, block) != 0)
            return -1;
        struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
        if (id_is_zero(hdr->parent_id))
            return 0;
        memcpy(cur, hdr->parent_id, 16);
    }
    errno = ELOOP;
    return -1;
}
