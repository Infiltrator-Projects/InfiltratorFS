// SPDX-License-Identifier: GPL-3.0-or-later
static int set_posix_tx(struct infs_volume *vol, const char *path,
                        uint32_t mask, uint32_t permissions,
                        uint32_t uid, uint32_t gid)
{
    if (!vol->writable) {
        errno = EROFS;
        return -1;
    }
    struct infs_lookup lookup;
    if (infs_lookup_path(vol, path, &lookup) != 0)
        return -1;
    uint8_t block[INFS_BLOCK_SIZE];
    if (read_object(vol, lookup.block, block) != 0)
        return -1;
    struct infs_posix_compat_disk *posix = object_posix(block);
    struct infs_attributes_disk *attributes = object_attributes(block);
    if (!posix || !attributes) {
        errno = EIO;
        return -1;
    }

    if (mask & INFS_POSIX_SET_PERMISSIONS)
        posix->permissions = infs_cpu_to_le32(permissions & 07777u);
    if (mask & INFS_POSIX_SET_UID)
        posix->uid = infs_cpu_to_le32(uid);
    if (mask & INFS_POSIX_SET_GID)
        posix->gid = infs_cpu_to_le32(gid);
    attributes->change_time_ns = (int64_t)infs_cpu_to_le64(
        (uint64_t)now_ns(vol));

    if (write_object(vol, lookup.block, block) != 0)
        return -1;
    return infs_volume_sync(vol);
}

int infs_set_posix_compat(struct infs_volume *vol, const char *path,
                          uint32_t mask, uint32_t permissions,
                          uint32_t uid, uint32_t gid)
{
    if ((mask & ~(INFS_POSIX_SET_PERMISSIONS | INFS_POSIX_SET_UID |
                  INFS_POSIX_SET_GID)) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (tx_begin(vol) != 0)
        return -1;
    int rc = set_posix_tx(vol, path, mask, permissions, uid, gid);
    if (rc != 0) {
        int saved = errno;
        tx_abort(vol);
        errno = saved;
    }
    return rc;
}

static int time_action_valid(uint32_t action)
{
    return action == INFS_TIME_OMIT || action == INFS_TIME_NOW ||
           action == INFS_TIME_SET;
}

static int set_times_tx(struct infs_volume *vol, const char *path,
                        const struct infs_time_update *update)
{
    if (!vol->writable) {
        errno = EROFS;
        return -1;
    }
    struct infs_lookup lookup;
    if (infs_lookup_path(vol, path, &lookup) != 0)
        return -1;
    uint8_t block[INFS_BLOCK_SIZE];
    if (read_object(vol, lookup.block, block) != 0)
        return -1;
    struct infs_attributes_disk *attributes = object_attributes(block);
    if (!attributes) {
        errno = EIO;
        return -1;
    }

    int64_t now = now_ns(vol);
    if (update->access_action == INFS_TIME_NOW)
        attributes->access_time_ns = (int64_t)infs_cpu_to_le64((uint64_t)now);
    else if (update->access_action == INFS_TIME_SET)
        attributes->access_time_ns = (int64_t)infs_cpu_to_le64(
            (uint64_t)update->access_time_ns);
    if (update->modification_action == INFS_TIME_NOW)
        attributes->modification_time_ns = (int64_t)infs_cpu_to_le64((uint64_t)now);
    else if (update->modification_action == INFS_TIME_SET)
        attributes->modification_time_ns = (int64_t)infs_cpu_to_le64(
            (uint64_t)update->modification_time_ns);
    attributes->change_time_ns = (int64_t)infs_cpu_to_le64((uint64_t)now);

    if (write_object(vol, lookup.block, block) != 0)
        return -1;
    return infs_volume_sync(vol);
}

int infs_set_times(struct infs_volume *vol, const char *path,
                   const struct infs_time_update *update)
{
    struct infs_time_update now = {
        .access_action = INFS_TIME_NOW,
        .modification_action = INFS_TIME_NOW,
    };
    if (!update)
        update = &now;
    if (!time_action_valid(update->access_action) ||
        !time_action_valid(update->modification_action)) {
        errno = EINVAL;
        return -1;
    }

    if (tx_begin(vol) != 0)
        return -1;
    int rc = set_times_tx(vol, path, update);
    if (rc != 0) {
        int saved = errno;
        tx_abort(vol);
        errno = saved;
    }
    return rc;
}
