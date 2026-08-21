// SPDX-License-Identifier: GPL-3.0-or-later
static int remove_object(struct infs_volume *vol, const char *path, int want_directory)
{
    struct infs_lookup parent;
    char name[INFS_NAME_MAX + 1u];
    infs_status status = split_parent(vol, path, &parent, name);
    if (status != INFS_STATUS_OK)
        return status;

    uint8_t parent_block[INFS_BLOCK_SIZE];
    status = read_object(vol, parent.block, parent_block);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_directory_payload_disk *pp;
    status = dir_validate(parent_block, &pp);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_dirent_disk de;
    status = dir_lookup_volume(vol, parent_block, name, &de);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_lookup child;
    status = index_find(vol, de.object_id, &child);
    if (status != INFS_STATUS_OK)
        return status;
    if (want_directory && child.type != INFS_OBJECT_DIRECTORY)
        return INFS_STATUS_NOT_DIRECTORY;
    if (!want_directory && child.type == INFS_OBJECT_DIRECTORY)
        return INFS_STATUS_IS_DIRECTORY;

    uint8_t child_block[INFS_BLOCK_SIZE];
    status = read_object(vol, child.block, child_block);
    if (status != INFS_STATUS_OK)
        return status;
    if (child.type == INFS_OBJECT_DIRECTORY) {
        struct infs_directory_payload_disk *cp;
        status = dir_validate(child_block, &cp);
        if (status != INFS_STATUS_OK)
            return status;
        if (infs_le32_to_cpu(cp->entry_count) != 0)
            return INFS_STATUS_NOT_EMPTY;
    } else if (child.type == INFS_OBJECT_FILE) {
        struct infs_file_payload_disk *fp;
        struct infs_extent_disk *ext;
        status = file_validate_volume(vol, child_block, &fp, &ext);
        if (status != INFS_STATUS_OK)
            return status;
        status = checksum_destroy_chain(vol, fp, child.object_id);
        if (status != INFS_STATUS_OK)
            return status;
        uint32_t count = infs_le32_to_cpu(fp->extent_count);
        for (uint32_t i = 0; i < count; ++i) {
            if (infs_le32_to_cpu(ext[i].flags) != INFS_EXTENT_NORMAL)
                continue;
            status = file_free_unshared_run(
                vol, child.object_id,
                infs_le64_to_cpu(ext[i].physical_block),
                infs_le32_to_cpu(ext[i].block_count));
            if (status != INFS_STATUS_OK)
                return status;
        }
    } else {
        return INFS_STATUS_CORRUPT;
    }

    status = dir_remove_volume(vol, parent_block, name, NULL);
    if (status != INFS_STATUS_OK)
        return status;
    if (child.type == INFS_OBJECT_DIRECTORY) {
        uint64_t nlink = infs_le64_to_cpu(pp->attributes.link_count);
        if (nlink <= 2u)
            return INFS_STATUS_CORRUPT;
        pp->attributes.link_count = infs_cpu_to_le64(nlink - 1u);
    }
    touch_mctime(vol, &pp->attributes);
    if (vol->tx_error != INFS_STATUS_OK)
        return vol->tx_error;
    status = write_object(vol, parent.block, parent_block);
    if (status != INFS_STATUS_OK)
        return status;
    status = index_remove(vol, child.object_id);
    if (status != INFS_STATUS_OK)
        return status;
    free_run(vol, child.block, 1);
    if (vol->tx_error != INFS_STATUS_OK)
        return vol->tx_error;
    return infs_volume_sync(vol);
}

infs_status infs_unlink(struct infs_volume *vol, const char *path)
{
    if (!vol || !path || path[0] != '/')
        return INFS_STATUS_INVALID_ARGUMENT;
    size_t length = strlen(path);
    if (length > 1u && path[length - 1u] == '/')
        return INFS_STATUS_NOT_DIRECTORY;

    infs_status status = tx_begin(vol);
    if (status != INFS_STATUS_OK)
        return status;
    status = remove_object(vol, path, 0);
    if (status != INFS_STATUS_OK)
        tx_abort(vol);
    return status;
}

infs_status infs_rmdir(struct infs_volume *vol, const char *path)
{
    if (!vol || !path || path[0] != '/')
        return INFS_STATUS_INVALID_ARGUMENT;
    infs_status status = tx_begin(vol);
    if (status != INFS_STATUS_OK)
        return status;
    status = remove_object(vol, path, 1);
    if (status != INFS_STATUS_OK)
        tx_abort(vol);
    return status;
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
        infs_status status = index_find(vol, cur, &lk);
        if (status != INFS_STATUS_OK)
            return status;
        uint8_t block[INFS_BLOCK_SIZE];
        status = read_object(vol, lk.block, block);
        if (status != INFS_STATUS_OK)
            return status;
        struct infs_object_header_disk *hdr =
            (struct infs_object_header_disk *)block;
        if (id_is_zero(hdr->parent_id))
            return 0;
        memcpy(cur, hdr->parent_id, 16);
    }
    return INFS_STATUS_LOOP_DETECTED;
}
