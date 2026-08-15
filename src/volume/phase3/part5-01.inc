// SPDX-License-Identifier: GPL-3.0-or-later
static int remove_object(struct infs_volume *vol, const char *path, int want_directory)
{
    struct infs_lookup parent;
    char name[INFS_NAME_MAX + 1u];
    if (split_parent(vol, path, &parent, name) != 0)
        return INFS_STATUS_ERROR;
    uint8_t parent_block[INFS_BLOCK_SIZE];
    if (read_object(vol, parent.block, parent_block) != 0)
        return INFS_STATUS_ERROR;
    struct infs_directory_payload_disk *pp;
    if (dir_validate(parent_block, &pp) != 0)
        return INFS_STATUS_ERROR;
    struct infs_dirent_disk *de = dir_find(pp, name, strlen(name), NULL);
    if (!de) {
        return INFS_STATUS_NOT_FOUND;
    }
    struct infs_lookup child;
    if (index_find(vol, de->object_id, &child) != 0)
        return INFS_STATUS_ERROR;
    if (want_directory && child.type != INFS_OBJECT_DIRECTORY) {
        return INFS_STATUS_NOT_DIRECTORY;
    }
    if (!want_directory && child.type == INFS_OBJECT_DIRECTORY) {
        return INFS_STATUS_IS_DIRECTORY;
    }

    uint8_t child_block[INFS_BLOCK_SIZE];
    if (read_object(vol, child.block, child_block) != 0)
        return INFS_STATUS_ERROR;
    if (child.type == INFS_OBJECT_DIRECTORY) {
        struct infs_directory_payload_disk *cp;
        if (dir_validate(child_block, &cp) != 0)
            return INFS_STATUS_ERROR;
        if (infs_le32_to_cpu(cp->entry_count) != 0) {
            return INFS_STATUS_NOT_EMPTY;
        }
    } else if (child.type == INFS_OBJECT_FILE) {
        struct infs_file_payload_disk *fp;
        struct infs_extent_disk *ext;
        if (file_validate_volume(vol, child_block, &fp, &ext) != 0)
            return INFS_STATUS_ERROR;
        if (checksum_destroy_chain(vol, fp, child.object_id) != 0)
            return INFS_STATUS_ERROR;
        uint32_t count = infs_le32_to_cpu(fp->extent_count);
        for (uint32_t i = 0; i < count; ++i)
            free_run(vol, infs_le64_to_cpu(ext[i].physical_block), infs_le32_to_cpu(ext[i].block_count));
    }

    if (dir_remove(parent_block, name, NULL) != 0)
        return INFS_STATUS_ERROR;
    if (child.type == INFS_OBJECT_DIRECTORY) {
        uint64_t nlink = infs_le64_to_cpu(pp->attributes.link_count);
        if (nlink > 2)
            pp->attributes.link_count = infs_cpu_to_le64(nlink - 1u);
    }
    touch_mctime(vol, &pp->attributes);
    if (write_object(vol, parent.block, parent_block) != 0)
        return INFS_STATUS_ERROR;
    if (index_remove(vol, child.object_id) != 0)
        return INFS_STATUS_ERROR;
    free_run(vol, child.block, 1);
    return infs_volume_sync(vol);
}

infs_status infs_unlink(struct infs_volume *vol, const char *path)
{
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
        if (index_find(vol, cur, &lk) != 0)
            return INFS_STATUS_ERROR;
        uint8_t block[INFS_BLOCK_SIZE];
        if (read_object(vol, lk.block, block) != 0)
            return INFS_STATUS_ERROR;
        struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
        if (id_is_zero(hdr->parent_id))
            return 0;
        memcpy(cur, hdr->parent_id, 16);
    }
    return INFS_STATUS_LOOP_DETECTED;
}
