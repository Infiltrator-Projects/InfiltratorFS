// SPDX-License-Identifier: GPL-3.0-or-later
static int create_object(struct infs_volume *vol, const char *path,
                         const struct infs_create_options *options,
                         uint16_t type)
{
    if (!vol->writable) {
        return INFS_STATUS_READ_ONLY;
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
    if (dir_find(parent_payload, name, strlen(name), NULL)) {
        return INFS_STATUS_ALREADY_EXISTS;
    }

    uint64_t object_block, got;
    status = alloc_run(vol, 1, &object_block, &got);
    if (status != INFS_STATUS_OK)
        return status;
    if (got != 1) {
        return INFS_STATUS_CORRUPT;
    }

    uint8_t object_id[16];
    status = infs_storage_random(&vol->storage, object_id, sizeof(object_id));
    if (status != INFS_STATUS_OK) {
        free_run(vol, object_block, 1);
        return status;
    }

    uint8_t object[INFS_BLOCK_SIZE];
    uint64_t n = (uint64_t)now_ns(vol);
    uint32_t payload_size;
    if (type == INFS_OBJECT_DIRECTORY)
        payload_size = sizeof(struct infs_directory_payload_disk);
    else
        payload_size = sizeof(struct infs_file_payload_disk);
    status = infs_object_init(object, type, object_id, parent.object_id,
                              infs_le64_to_cpu(vol->sb.generation) + 1u,
                              payload_size);
    if (status != INFS_STATUS_OK) {
        free_run(vol, object_block, 1);
        return status;
    }

    struct infs_attributes_disk *attributes;
    struct infs_posix_compat_disk *posix;
    if (type == INFS_OBJECT_DIRECTORY) {
        struct infs_directory_payload_disk *p =
            (struct infs_directory_payload_disk *)(object + sizeof(struct infs_object_header_disk));
        memset(p, 0, sizeof(*p));
        attributes = &p->attributes;
        posix = &p->posix;
        attributes->link_count = infs_cpu_to_le64(2);
    } else {
        struct infs_file_payload_disk *p =
            (struct infs_file_payload_disk *)(object + sizeof(struct infs_object_header_disk));
        memset(p, 0, sizeof(*p));
        p->data_checksum_type = infs_cpu_to_le32(INFS_CHECKSUM_SHA256);
        attributes = &p->attributes;
        posix = &p->posix;
        attributes->link_count = infs_cpu_to_le64(1);
    }
    attributes->portable_flags = infs_cpu_to_le64(options ? options->portable_flags : 0);
    attributes->birth_time_ns = (int64_t)infs_cpu_to_le64(n);
    attributes->access_time_ns = (int64_t)infs_cpu_to_le64(n);
    attributes->modification_time_ns = (int64_t)infs_cpu_to_le64(n);
    attributes->change_time_ns = (int64_t)infs_cpu_to_le64(n);
    posix->permissions = infs_cpu_to_le32(
        (options ? options->posix_permissions :
         (type == INFS_OBJECT_DIRECTORY ? 0755u : 0644u)) & 07777u);
    posix->uid = infs_cpu_to_le32(options ? options->posix_uid : 0);
    posix->gid = infs_cpu_to_le32(options ? options->posix_gid : 0);

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
    status = dir_add(parent_block, name, object_id, type);
    if (status != INFS_STATUS_OK) {
        (void)index_remove(vol, object_id);
        free_run(vol, object_block, 1);
        return status;
    }
    if (type == INFS_OBJECT_DIRECTORY)
        parent_payload->attributes.link_count = infs_cpu_to_le64(
            infs_le64_to_cpu(parent_payload->attributes.link_count) + 1u);
    touch_mctime(vol, &parent_payload->attributes);
    status = write_object(vol, parent.block, parent_block);
    if (status != INFS_STATUS_OK)
        return status;
    return infs_volume_sync(vol);
}
