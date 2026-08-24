// SPDX-License-Identifier: GPL-3.0-or-later
static int destroy_replaced_object_tx(struct infs_volume *vol,
                                      const struct infs_lookup *target)
{
    uint8_t object[INFS_BLOCK_SIZE];
    infs_status status = read_object(vol, target->block, object);
    if (status != INFS_STATUS_OK)
        return status;

    if (target->type == INFS_OBJECT_DIRECTORY) {
        struct infs_directory_payload_disk *directory;
        status = dir_validate(object, &directory);
        if (status != INFS_STATUS_OK)
            return status;
        if (infs_le32_to_cpu(directory->entry_count) != 0)
            return INFS_STATUS_NOT_EMPTY;
    } else if (target->type == INFS_OBJECT_FILE) {
        struct infs_file_payload_disk *file;
        struct infs_extent_disk *extents;
        status = file_validate_volume(vol, object, &file, &extents);
        if (status != INFS_STATUS_OK)
            return status;
        status = checksum_destroy_chain(vol, file, target->object_id);
        if (status != INFS_STATUS_OK)
            return status;
        uint32_t count = infs_le32_to_cpu(file->extent_count);
        for (uint32_t i = 0; i < count; ++i) {
            if (infs_le32_to_cpu(extents[i].flags) != INFS_EXTENT_NORMAL)
                continue;
            status = file_free_unshared_run(
                vol, target->object_id,
                infs_le64_to_cpu(extents[i].physical_block),
                infs_le32_to_cpu(extents[i].block_count));
            if (status != INFS_STATUS_OK)
                return status;
        }
    } else if (target->type == INFS_OBJECT_SYMLINK) {
        status = symlink_validate(vol, object, NULL, NULL);
        if (status != INFS_STATUS_OK)
            return status;
    } else {
        return INFS_STATUS_CORRUPT;
    }

    status = index_remove(vol, target->object_id);
    if (status != INFS_STATUS_OK)
        return status;
    free_run(vol, target->block, 1);
    return vol->tx_error;
}

static int rename_tx(struct infs_volume *vol, const char *oldpath,
                     const char *newpath)
{
    if (!vol->writable)
        return INFS_STATUS_READ_ONLY;

    struct infs_lookup old_parent, new_parent;
    char old_name[INFS_NAME_MAX + 1u], new_name[INFS_NAME_MAX + 1u];
    infs_status status = split_parent(vol, oldpath, &old_parent, old_name);
    if (status != INFS_STATUS_OK)
        return status;
    status = split_parent(vol, newpath, &new_parent, new_name);
    if (status != INFS_STATUS_OK)
        return status;

    uint8_t old_block[INFS_BLOCK_SIZE], new_block[INFS_BLOCK_SIZE];
    status = read_object(vol, old_parent.block, old_block);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_directory_payload_disk *old_payload;
    status = dir_validate(old_block, &old_payload);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_dirent_disk source_entry;
    status = dir_lookup_volume(vol, old_block, old_name, &source_entry);
    if (status != INFS_STATUS_OK)
        return status;

    uint8_t source_id[16];
    memcpy(source_id, source_entry.object_id, 16);
    uint16_t source_type = infs_le16_to_cpu(source_entry.object_type);
    struct infs_lookup source;
    status = index_find(vol, source_id, &source);
    if (status != INFS_STATUS_OK)
        return status;
    if (source.type != source_type)
        return INFS_STATUS_CORRUPT;

    int same_parent = memcmp(old_parent.object_id, new_parent.object_id, 16) == 0;
    if (same_parent) {
        memcpy(new_block, old_block, sizeof(new_block));
    } else {
        status = read_object(vol, new_parent.block, new_block);
        if (status != INFS_STATUS_OK)
            return status;
    }
    struct infs_directory_payload_disk *new_payload;
    status = dir_validate(new_block, &new_payload);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_dirent_disk destination_entry;
    status = dir_lookup_volume(vol, new_block, new_name, &destination_entry);
    int have_destination = status == INFS_STATUS_OK;
    if (!have_destination && status != INFS_STATUS_NOT_FOUND)
        return status;

    struct infs_lookup destination;
    uint16_t destination_type = 0;
    if (have_destination) {
        if (memcmp(destination_entry.object_id, source_id, 16) == 0)
            return INFS_STATUS_OK;
        destination_type = infs_le16_to_cpu(destination_entry.object_type);
        status = index_find(vol, destination_entry.object_id, &destination);
        if (status != INFS_STATUS_OK)
            return status;
        if (destination.type != destination_type)
            return INFS_STATUS_CORRUPT;
        if (source_type != INFS_OBJECT_DIRECTORY &&
            destination_type == INFS_OBJECT_DIRECTORY)
            return INFS_STATUS_IS_DIRECTORY;
        if (source_type == INFS_OBJECT_DIRECTORY &&
            destination_type != INFS_OBJECT_DIRECTORY)
            return INFS_STATUS_NOT_DIRECTORY;
    }

    if (source_type == INFS_OBJECT_DIRECTORY) {
        int ancestor = id_is_ancestor(vol, source_id, new_parent.object_id);
        if (ancestor < 0)
            return ancestor;
        if (ancestor)
            return INFS_STATUS_INVALID_ARGUMENT;
    }

    if (have_destination) {
        status = destroy_replaced_object_tx(vol, &destination);
        if (status != INFS_STATUS_OK)
            return status;
        status = dir_remove_volume(vol, new_block, new_name, NULL);
        if (status != INFS_STATUS_OK)
            return status;
        new_payload = (struct infs_directory_payload_disk *)(
            new_block + sizeof(struct infs_object_header_disk));
        if (destination_type == INFS_OBJECT_DIRECTORY) {
            uint64_t links = infs_le64_to_cpu(new_payload->attributes.link_count);
            if (links <= 2u)
                return INFS_STATUS_CORRUPT;
            new_payload->attributes.link_count = infs_cpu_to_le64(links - 1u);
        }
        status = infs_object_finalize(new_block);
        if (status != INFS_STATUS_OK)
            return status;
    }

    if (same_parent) {
        status = dir_remove_volume(vol, new_block, old_name, NULL);
        if (status != INFS_STATUS_OK)
            return status;
        status = infs_object_finalize(new_block);
        if (status != INFS_STATUS_OK)
            return status;
        status = dir_add_volume(vol, new_block, new_name, source_id, source_type);
        if (status != INFS_STATUS_OK)
            return status;
        new_payload = (struct infs_directory_payload_disk *)(
            new_block + sizeof(struct infs_object_header_disk));
        touch_mctime(vol, &new_payload->attributes);
        if (vol->tx_error != INFS_STATUS_OK)
            return vol->tx_error;
        status = write_object(vol, old_parent.block, new_block);
        if (status != INFS_STATUS_OK)
            return status;
    } else {
        status = dir_add_volume(vol, new_block, new_name, source_id, source_type);
        if (status != INFS_STATUS_OK)
            return status;
        status = dir_remove_volume(vol, old_block, old_name, NULL);
        if (status != INFS_STATUS_OK)
            return status;

        old_payload = (struct infs_directory_payload_disk *)(
            old_block + sizeof(struct infs_object_header_disk));
        new_payload = (struct infs_directory_payload_disk *)(
            new_block + sizeof(struct infs_object_header_disk));
        if (source_type == INFS_OBJECT_DIRECTORY) {
            uint64_t old_links = infs_le64_to_cpu(old_payload->attributes.link_count);
            if (old_links <= 2u)
                return INFS_STATUS_CORRUPT;
            old_payload->attributes.link_count = infs_cpu_to_le64(old_links - 1u);
            new_payload->attributes.link_count = infs_cpu_to_le64(
                infs_le64_to_cpu(new_payload->attributes.link_count) + 1u);
        }
        touch_mctime(vol, &old_payload->attributes);
        touch_mctime(vol, &new_payload->attributes);
        if (vol->tx_error != INFS_STATUS_OK)
            return vol->tx_error;
        status = write_object(vol, old_parent.block, old_block);
        if (status != INFS_STATUS_OK)
            return status;
        status = write_object(vol, new_parent.block, new_block);
        if (status != INFS_STATUS_OK)
            return status;
    }

    struct infs_lookup current_source;
    status = index_find(vol, source_id, &current_source);
    if (status != INFS_STATUS_OK)
        return status;
    uint8_t source_block[INFS_BLOCK_SIZE];
    status = read_object(vol, current_source.block, source_block);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_object_header_disk *source_header =
        (struct infs_object_header_disk *)source_block;
    if (!same_parent)
        memcpy(source_header->parent_id, new_parent.object_id, 16);
    struct infs_attributes_disk *source_attributes = object_attributes(source_block);
    if (!source_attributes)
        return INFS_STATUS_CORRUPT;

    int64_t now = 0;
    status = infs_storage_current_time_ns(&vol->storage, &now);
    if (status != INFS_STATUS_OK)
        return status;
    source_attributes->change_time_ns =
        (int64_t)infs_cpu_to_le64((uint64_t)now);
    status = write_object(vol, current_source.block, source_block);
    if (status != INFS_STATUS_OK)
        return status;

    return finish_mutation(vol, (uint64_t)INFS_BLOCK_SIZE * 6u);
}

static infs_status validate_rename_trailing_slash(struct infs_volume *vol,
                                                  const char *path)
{
    size_t length = 0;
    infs_status status = path_length_checked(path, &length);
    if (status != INFS_STATUS_OK)
        return status;
    if (length <= 1u || path[length - 1u] != '/')
        return INFS_STATUS_OK;

    struct infs_lookup lookup;
    status = infs_lookup_path(vol, path, &lookup);
    if (status != INFS_STATUS_OK)
        return status;
    return lookup.type == INFS_OBJECT_DIRECTORY ?
        INFS_STATUS_OK : INFS_STATUS_NOT_DIRECTORY;
}

infs_status infs_rename(struct infs_volume *vol, const char *oldpath,
                        const char *newpath)
{
    if (!vol || !oldpath || !newpath || oldpath[0] != '/' || newpath[0] != '/')
        return INFS_STATUS_INVALID_ARGUMENT;

    infs_status status = validate_rename_trailing_slash(vol, oldpath);
    if (status != INFS_STATUS_OK)
        return status;
    status = validate_rename_trailing_slash(vol, newpath);
    if (status != INFS_STATUS_OK)
        return status;

    status = tx_begin(vol);
    if (status != INFS_STATUS_OK)
        return status;
    status = rename_tx(vol, oldpath, newpath);
    if (status != INFS_STATUS_OK)
        tx_abort(vol);
    return status;
}
