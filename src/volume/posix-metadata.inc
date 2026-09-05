// SPDX-License-Identifier: GPL-3.0-or-later
static int set_posix_tx(struct infs_volume *vol, const char *path,
                        uint32_t mask, uint32_t permissions,
                        uint32_t uid, uint32_t gid)
{
    if (!vol->writable)
        return INFS_STATUS_READ_ONLY;
    struct infs_lookup lookup;
    infs_status status = infs_lookup_path(vol, path, &lookup);
    if (status != INFS_STATUS_OK) return status;
    uint8_t block[INFS_BLOCK_SIZE];
    status = read_object(vol, lookup.block, block);
    if (status != INFS_STATUS_OK) return status;
    struct infs_posix_compat_disk *posix = object_posix(block);
    struct infs_attributes_disk *attributes = object_attributes(block);
    if (!posix || !attributes) return INFS_STATUS_CORRUPT;
    struct infs_timestamp now = {0};
    status = infs_storage_current_time(&vol->storage, &now);
    if (status != INFS_STATUS_OK) return status;
    if (mask & INFS_POSIX_SET_PERMISSIONS)
        posix->permissions = infs_cpu_to_le32(permissions & 07777u);
    if (mask & INFS_POSIX_SET_UID) posix->uid = infs_cpu_to_le32(uid);
    if (mask & INFS_POSIX_SET_GID) posix->gid = infs_cpu_to_le32(gid);
    timestamp_disk_from_value(&attributes->change_time, &now);
    status = write_object(vol, lookup.block, block);
    if (status != INFS_STATUS_OK) return status;
    return finish_mutation(vol, (uint64_t)INFS_BLOCK_SIZE * 2u);
}

infs_status infs_set_posix_compat(struct infs_volume *vol, const char *path,
                                  uint32_t mask, uint32_t permissions,
                                  uint32_t uid, uint32_t gid)
{
    if (!vol || !path) return INFS_STATUS_INVALID_ARGUMENT;
    if ((mask & ~(INFS_POSIX_SET_PERMISSIONS | INFS_POSIX_SET_UID |
                  INFS_POSIX_SET_GID)) != 0)
        return INFS_STATUS_INVALID_ARGUMENT;
    int started_transaction = 0;
    infs_status status = mutation_scope_begin(vol, &started_transaction);
    if (status != INFS_STATUS_OK) return status;
    status = set_posix_tx(vol, path, mask, permissions, uid, gid);
    if (status != INFS_STATUS_OK) mutation_scope_fail(vol, started_transaction);
    else mutation_scope_success(vol);
    return status;
}

static int time_action_valid(uint32_t action)
{
    return action == INFS_TIME_OMIT || action == INFS_TIME_NOW ||
           action == INFS_TIME_SET;
}

static int set_times_tx(struct infs_volume *vol, const char *path,
                        const struct infs_time_update *update)
{
    if (!vol->writable) return INFS_STATUS_READ_ONLY;
    int changed = update->birth_action != INFS_TIME_OMIT ||
        update->access_action != INFS_TIME_OMIT ||
        update->modification_action != INFS_TIME_OMIT ||
        update->change_action != INFS_TIME_OMIT;
    if (!changed) return INFS_STATUS_OK;
    struct infs_lookup lookup;
    infs_status status = infs_lookup_path(vol, path, &lookup);
    if (status != INFS_STATUS_OK) return status;
    uint8_t block[INFS_BLOCK_SIZE];
    status = read_object(vol, lookup.block, block);
    if (status != INFS_STATUS_OK) return status;
    struct infs_attributes_disk *attributes = object_attributes(block);
    if (!attributes) return INFS_STATUS_CORRUPT;

    int needs_now = update->birth_action == INFS_TIME_NOW ||
        update->access_action == INFS_TIME_NOW ||
        update->modification_action == INFS_TIME_NOW ||
        update->change_action == INFS_TIME_NOW ||
        (update->change_action == INFS_TIME_OMIT &&
         (update->birth_action != INFS_TIME_OMIT ||
          update->access_action != INFS_TIME_OMIT ||
          update->modification_action != INFS_TIME_OMIT));
    struct infs_timestamp now = {0};
    if (needs_now) {
        status = infs_storage_current_time(&vol->storage, &now);
        if (status != INFS_STATUS_OK) return status;
    }
#define APPLY_TIME(field, action, value) do { \
    if ((action) == INFS_TIME_NOW) timestamp_disk_from_value(&(field), &now); \
    else if ((action) == INFS_TIME_SET) timestamp_disk_from_value(&(field), &(value)); \
} while (0)
    APPLY_TIME(attributes->birth_time, update->birth_action, update->birth_time);
    APPLY_TIME(attributes->access_time, update->access_action, update->access_time);
    APPLY_TIME(attributes->modification_time, update->modification_action,
               update->modification_time);
    if (update->change_action == INFS_TIME_OMIT)
        timestamp_disk_from_value(&attributes->change_time, &now);
    else
        APPLY_TIME(attributes->change_time, update->change_action, update->change_time);
#undef APPLY_TIME
    status = write_object(vol, lookup.block, block);
    if (status != INFS_STATUS_OK) return status;
    return finish_mutation(vol, (uint64_t)INFS_BLOCK_SIZE * 2u);
}

infs_status infs_set_times(struct infs_volume *vol, const char *path,
                           const struct infs_time_update *update)
{
    if (!vol || !path) return INFS_STATUS_INVALID_ARGUMENT;
    struct infs_time_update now = {
        .access_action = INFS_TIME_NOW,
        .modification_action = INFS_TIME_NOW,
    };
    if (!update) update = &now;
    if (!time_action_valid(update->birth_action) ||
        !time_action_valid(update->access_action) ||
        !time_action_valid(update->modification_action) ||
        !time_action_valid(update->change_action))
        return INFS_STATUS_INVALID_ARGUMENT;
    if ((update->birth_action == INFS_TIME_SET && !infs_timestamp_valid(&update->birth_time)) ||
        (update->access_action == INFS_TIME_SET && !infs_timestamp_valid(&update->access_time)) ||
        (update->modification_action == INFS_TIME_SET && !infs_timestamp_valid(&update->modification_time)) ||
        (update->change_action == INFS_TIME_SET && !infs_timestamp_valid(&update->change_time)))
        return INFS_STATUS_INVALID_ARGUMENT;
    int started_transaction = 0;
    infs_status status = mutation_scope_begin(vol, &started_transaction);
    if (status != INFS_STATUS_OK) return status;
    status = set_times_tx(vol, path, update);
    if (status != INFS_STATUS_OK) mutation_scope_fail(vol, started_transaction);
    else mutation_scope_success(vol);
    return status;
}

static int set_portable_flags_tx(struct infs_volume *vol, const char *path,
                                 uint64_t portable_flags)
{
    if (!vol->writable) return INFS_STATUS_READ_ONLY;
    struct infs_lookup lookup;
    infs_status status = infs_lookup_path(vol, path, &lookup);
    if (status != INFS_STATUS_OK) return status;
    uint8_t block[INFS_BLOCK_SIZE];
    status = read_object(vol, lookup.block, block);
    if (status != INFS_STATUS_OK) return status;
    struct infs_attributes_disk *attributes = object_attributes(block);
    if (!attributes) return INFS_STATUS_CORRUPT;
    struct infs_timestamp now = {0};
    status = infs_storage_current_time(&vol->storage, &now);
    if (status != INFS_STATUS_OK) return status;
    attributes->portable_flags = infs_cpu_to_le64(portable_flags);
    timestamp_disk_from_value(&attributes->change_time, &now);
    status = write_object(vol, lookup.block, block);
    if (status != INFS_STATUS_OK) return status;
    return finish_mutation(vol, (uint64_t)INFS_BLOCK_SIZE * 2u);
}

infs_status infs_set_portable_flags(struct infs_volume *vol, const char *path,
                                    uint64_t portable_flags)
{
    if (!vol || !path || (portable_flags & ~INFS_KNOWN_ATTR_FLAGS) != 0)
        return INFS_STATUS_INVALID_ARGUMENT;
    int started_transaction = 0;
    infs_status status = mutation_scope_begin(vol, &started_transaction);
    if (status != INFS_STATUS_OK) return status;
    status = set_portable_flags_tx(vol, path, portable_flags);
    if (status != INFS_STATUS_OK) mutation_scope_fail(vol, started_transaction);
    else mutation_scope_success(vol);
    return status;
}
