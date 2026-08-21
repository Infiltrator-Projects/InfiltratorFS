// SPDX-License-Identifier: GPL-3.0-or-later
infs_status infs_get_attributes(struct infs_volume *vol, const char *path,
                                struct infs_attributes *attributes)
{
    if (!attributes)
        return INFS_STATUS_INVALID_ARGUMENT;
    struct infs_lookup lk;
    infs_status status = infs_lookup_path(vol, path, &lk);
    if (status != INFS_STATUS_OK)
        return status;
    uint8_t block[INFS_BLOCK_SIZE];
    status = read_object(vol, lk.block, block);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_attributes_disk *disk_attributes = object_attributes(block);
    if (!disk_attributes)
        return INFS_STATUS_CORRUPT;
    memset(attributes, 0, sizeof(*attributes));
    memcpy(attributes->object_id, lk.object_id, sizeof(attributes->object_id));
    attributes->object_type = lk.type;
    attributes->logical_size = infs_le64_to_cpu(disk_attributes->logical_size);
    attributes->link_count = infs_le64_to_cpu(disk_attributes->link_count);
    attributes->portable_flags = infs_le64_to_cpu(disk_attributes->portable_flags);
    attributes->birth_time_ns = (int64_t)infs_le64_to_cpu(
        (uint64_t)disk_attributes->birth_time_ns);
    attributes->access_time_ns = (int64_t)infs_le64_to_cpu(
        (uint64_t)disk_attributes->access_time_ns);
    attributes->modification_time_ns = (int64_t)infs_le64_to_cpu(
        (uint64_t)disk_attributes->modification_time_ns);
    attributes->change_time_ns = (int64_t)infs_le64_to_cpu(
        (uint64_t)disk_attributes->change_time_ns);
    memcpy(attributes->security_object_id, disk_attributes->security_object_id, 16);
    memcpy(attributes->extended_attributes_object_id,
           disk_attributes->extended_attributes_object_id, 16);

    const struct infs_posix_compat_disk *posix;
    if (lk.type == INFS_OBJECT_FILE) {
        struct infs_file_payload_disk *p;
        struct infs_extent_disk *ext;
        status = file_validate_volume(vol, block, &p, &ext);
        if (status != INFS_STATUS_OK)
            return status;
        attributes->allocated_size =
            file_allocated_blocks(p, ext) * INFS_BLOCK_SIZE;
        posix = &p->posix;
    } else {
        const struct infs_object_header_disk *header =
            (const struct infs_object_header_disk *)block;
        const struct infs_directory_payload_disk *p =
            (const struct infs_directory_payload_disk *)(header + 1);
        uint64_t blocks = 1u;
        if (infs_le16_to_cpu(header->object_version) ==
            INFS_OBJECT_VERSION_PAGED)
            blocks += infs_le32_to_cpu(p->bytes_used);
        attributes->allocated_size = blocks * INFS_BLOCK_SIZE;
        posix = &p->posix;
    }
    attributes->posix_permissions = infs_le32_to_cpu(posix->permissions);
    attributes->posix_uid = infs_le32_to_cpu(posix->uid);
    attributes->posix_gid = infs_le32_to_cpu(posix->gid);
    return INFS_STATUS_OK;
}

infs_status infs_list_dir(struct infs_volume *vol, const char *path,
                          struct infs_dir_item **items, size_t *count_out)
{
    if (!items || !count_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    *items = NULL;
    *count_out = 0;

    struct infs_lookup lk;
    infs_status status = infs_lookup_path(vol, path, &lk);
    if (status != INFS_STATUS_OK)
        return status;
    if (lk.type != INFS_OBJECT_DIRECTORY)
        return INFS_STATUS_NOT_DIRECTORY;

    uint8_t block[INFS_BLOCK_SIZE];
    status = read_object(vol, lk.block, block);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_directory_payload_disk *payload;
    status = dir_validate(block, &payload);
    if (status != INFS_STATUS_OK)
        return status;

    uint32_t count = infs_le32_to_cpu(payload->entry_count);
    struct infs_dir_item *list = calloc(count ? count : 1u, sizeof(*list));
    if (!list)
        return INFS_STATUS_NO_MEMORY;

    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)block;
    if (infs_le16_to_cpu(header->object_version) ==
        INFS_OBJECT_VERSION_PAGED) {
        uint8_t *snapshot = NULL;
        size_t bytes = 0;
        uint32_t snapshot_count = 0;
        status = paged_dir_snapshot(vol, block, &snapshot, &bytes,
                                    &snapshot_count);
        if (status != INFS_STATUS_OK) {
            free(list);
            return status;
        }
        if (snapshot_count != count) {
            free(snapshot);
            free(list);
            return INFS_STATUS_CORRUPT;
        }
        size_t offset = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (offset >= bytes) {
                free(snapshot);
                free(list);
                return INFS_STATUS_CORRUPT;
            }
            struct infs_dirent_disk *entry =
                (struct infs_dirent_disk *)(snapshot + offset);
            uint16_t record_size = infs_le16_to_cpu(entry->record_size);
            uint16_t name_length = infs_le16_to_cpu(entry->name_length);
            memcpy(list[i].name, snapshot + offset + sizeof(*entry),
                   name_length);
            list[i].name[name_length] = '\0';
            memcpy(list[i].object_id, entry->object_id, 16);
            list[i].type = infs_le16_to_cpu(entry->object_type);
            offset += record_size;
        }
        if (offset != bytes) {
            free(snapshot);
            free(list);
            return INFS_STATUS_CORRUPT;
        }
        free(snapshot);
    } else {
        uint8_t *p = (uint8_t *)(payload + 1);
        for (uint32_t i = 0; i < count; ++i) {
            struct infs_dirent_disk *e = (struct infs_dirent_disk *)p;
            uint16_t n = infs_le16_to_cpu(e->name_length);
            memcpy(list[i].name, p + sizeof(*e), n);
            list[i].name[n] = '\0';
            memcpy(list[i].object_id, e->object_id, 16);
            list[i].type = infs_le16_to_cpu(e->object_type);
            p += infs_le16_to_cpu(e->record_size);
        }
    }
    *items = list;
    *count_out = count;
    return INFS_STATUS_OK;
}

void infs_free_dir_items(struct infs_dir_item *items)
{
    free(items);
}

infs_status infs_create_file(struct infs_volume *vol, const char *path,
                             const struct infs_create_options *options)
{
    if (!vol || !path || path[0] != '/')
        return INFS_STATUS_INVALID_ARGUMENT;
    size_t length = strlen(path);
    if (length > 1u && path[length - 1u] == '/')
        return INFS_STATUS_NOT_DIRECTORY;

    infs_status status = tx_begin(vol);
    if (status != INFS_STATUS_OK)
        return status;
    status = create_object(vol, path, options, INFS_OBJECT_FILE);
    if (status == INFS_STATUS_OK)
        status = infs_volume_sync(vol);
    if (status != INFS_STATUS_OK)
        tx_abort(vol);
    return status;
}

infs_status infs_mkdir(struct infs_volume *vol, const char *path,
                       const struct infs_create_options *options)
{
    if (!vol || !path || path[0] != '/')
        return INFS_STATUS_INVALID_ARGUMENT;
    infs_status status = tx_begin(vol);
    if (status != INFS_STATUS_OK)
        return status;
    status = create_object(vol, path, options, INFS_OBJECT_DIRECTORY);
    if (status == INFS_STATUS_OK)
        status = infs_volume_sync(vol);
    if (status != INFS_STATUS_OK)
        tx_abort(vol);
    return status;
}
