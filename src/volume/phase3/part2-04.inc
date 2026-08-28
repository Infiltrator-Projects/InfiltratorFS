// SPDX-License-Identifier: GPL-3.0-or-later
static int create_object(struct infs_volume *vol, const char *path,
                         const struct infs_create_options *options,
                         uint16_t type, const char *symlink_target)
{
    if (!vol->writable)
        return INFS_STATUS_READ_ONLY;
    if (options && (options->portable_flags & ~INFS_KNOWN_ATTR_FLAGS) != 0)
        return INFS_STATUS_INVALID_ARGUMENT;

    size_t symlink_length = 0;
    if (type == INFS_OBJECT_SYMLINK) {
        if (!symbolic_links_enabled(vol))
            return INFS_STATUS_NOT_SUPPORTED;
        if (!symlink_target)
            return INFS_STATUS_INVALID_ARGUMENT;
        while (symlink_length <= INFS_SYMLINK_TARGET_MAX &&
               symlink_target[symlink_length] != '\0')
            ++symlink_length;
        if (symlink_length == 0 || symlink_length > INFS_SYMLINK_TARGET_MAX ||
            !infs_utf8_validate(symlink_target, symlink_length))
            return symlink_length > INFS_SYMLINK_TARGET_MAX ?
                INFS_STATUS_NAME_TOO_LONG : INFS_STATUS_INVALID_ARGUMENT;
    } else if (type != INFS_OBJECT_DIRECTORY && type != INFS_OBJECT_FILE) {
        return INFS_STATUS_INVALID_ARGUMENT;
    }

    struct infs_lookup parent;
    char name[INFS_NAME_MAX + 1u];
    infs_status status = split_parent(vol, path, &parent, name);
    if (status != INFS_STATUS_OK)
        return status;

    uint8_t parent_block[INFS_BLOCK_SIZE];
    status = read_object(vol, parent.block, parent_block);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_directory_payload_disk *parent_payload;
    status = dir_validate(parent_block, &parent_payload);
    if (status != INFS_STATUS_OK)
        return status;
    status = dir_lookup_volume(vol, parent_block, name, NULL);
    if (status == INFS_STATUS_OK)
        return INFS_STATUS_ALREADY_EXISTS;
    if (status != INFS_STATUS_NOT_FOUND)
        return status;

    uint64_t object_block, got;
    status = alloc_run(vol, 1, &object_block, &got);
    if (status != INFS_STATUS_OK)
        return status;
    if (got != 1)
        return INFS_STATUS_CORRUPT;

    uint8_t object_id[16];
    status = generate_unique_object_id(vol, object_id);
    if (status != INFS_STATUS_OK) {
        free_run(vol, object_block, 1);
        return status;
    }

    int64_t now = 0;
    status = infs_storage_current_time_ns(&vol->storage, &now);
    if (status != INFS_STATUS_OK) {
        free_run(vol, object_block, 1);
        return status;
    }
    uint64_t n = (uint64_t)now;

    uint8_t object[INFS_BLOCK_SIZE];
    uint32_t payload_size;
    int directory_tree =
        type == INFS_OBJECT_DIRECTORY &&
        (infs_le64_to_cpu(vol->sb.incompat_flags) &
         INFS_INCOMPAT_DIRECTORY_TREE) != 0;
    if (type == INFS_OBJECT_DIRECTORY)
        payload_size = (uint32_t)(sizeof(struct infs_directory_payload_disk) +
            (directory_tree ? sizeof(uint64_t) : 0u));
    else if (type == INFS_OBJECT_FILE)
        payload_size = sizeof(struct infs_file_payload_disk);
    else
        payload_size = (uint32_t)(
            sizeof(struct infs_symlink_payload_disk) + symlink_length);
    status = infs_object_init(object, type, object_id, parent.object_id,
                              infs_le64_to_cpu(vol->sb.generation) + 1u,
                              payload_size);
    if (status != INFS_STATUS_OK) {
        free_run(vol, object_block, 1);
        return status;
    }
    if (type == INFS_OBJECT_DIRECTORY) {
        struct infs_object_header_disk *header =
            (struct infs_object_header_disk *)object;
        if (directory_tree)
            header->object_version =
                infs_cpu_to_le16(INFS_OBJECT_VERSION_TREE);
        else if ((infs_le64_to_cpu(vol->sb.incompat_flags) &
                  INFS_INCOMPAT_PAGED_METADATA) != 0)
            header->object_version =
                infs_cpu_to_le16(INFS_OBJECT_VERSION_PAGED);
    }

    struct infs_attributes_disk *attributes;
    struct infs_posix_compat_disk *posix;
    if (type == INFS_OBJECT_DIRECTORY) {
        struct infs_directory_payload_disk *p =
            (struct infs_directory_payload_disk *)(
                object + sizeof(struct infs_object_header_disk));
        memset(p, 0, sizeof(*p));
        attributes = &p->attributes;
        posix = &p->posix;
        attributes->link_count = infs_cpu_to_le64(2);
    } else if (type == INFS_OBJECT_FILE) {
        struct infs_file_payload_disk *p =
            (struct infs_file_payload_disk *)(
                object + sizeof(struct infs_object_header_disk));
        memset(p, 0, sizeof(*p));
        p->data_checksum_type = infs_cpu_to_le32(INFS_CHECKSUM_SHA256);
        attributes = &p->attributes;
        posix = &p->posix;
        attributes->link_count = infs_cpu_to_le64(1);
    } else {
        struct infs_symlink_payload_disk *p =
            (struct infs_symlink_payload_disk *)(
                object + sizeof(struct infs_object_header_disk));
        memset(p, 0, sizeof(*p));
        attributes = &p->attributes;
        posix = &p->posix;
        attributes->logical_size = infs_cpu_to_le64(symlink_length);
        attributes->link_count = infs_cpu_to_le64(1);
        p->target_length = infs_cpu_to_le32((uint32_t)symlink_length);
        memcpy(p + 1, symlink_target, symlink_length);
    }
    attributes->portable_flags = infs_cpu_to_le64(
        options ? options->portable_flags : 0);
    attributes->birth_time_ns = (int64_t)infs_cpu_to_le64(n);
    attributes->access_time_ns = (int64_t)infs_cpu_to_le64(n);
    attributes->modification_time_ns = (int64_t)infs_cpu_to_le64(n);
    attributes->change_time_ns = (int64_t)infs_cpu_to_le64(n);
    posix->permissions = infs_cpu_to_le32(
        (options ? options->posix_permissions :
         (type == INFS_OBJECT_DIRECTORY ? 0755u :
          type == INFS_OBJECT_SYMLINK ? 0777u : 0644u)) & 07777u);
    posix->uid = infs_cpu_to_le32(options ? options->posix_uid : 0);
    posix->gid = infs_cpu_to_le32(options ? options->posix_gid : 0);
    posix->flags = 0;

    status = infs_object_finalize(object);
    if (status == INFS_STATUS_OK)
        status = write_block(vol, object_block, object);
    if (status != INFS_STATUS_OK) {
        free_run(vol, object_block, 1);
        return status;
    }
    status = index_add(vol, object_id, object_block, type);
    if (status != INFS_STATUS_OK) {
        free_run(vol, object_block, 1);
        return status;
    }
    status = dir_add_volume(vol, parent_block, name, object_id, type);
    if (status != INFS_STATUS_OK) {
        (void)index_remove(vol, object_id);
        free_run(vol, object_block, 1);
        return status;
    }
    if (type == INFS_OBJECT_DIRECTORY)
        parent_payload->attributes.link_count = infs_cpu_to_le64(
            infs_le64_to_cpu(parent_payload->attributes.link_count) + 1u);
    touch_mctime(vol, &parent_payload->attributes);
    if (vol->tx_error != INFS_STATUS_OK)
        return vol->tx_error;
    status = write_object(vol, parent.block, parent_block);
    if (status != INFS_STATUS_OK)
        return status;
    return INFS_STATUS_OK;
}
