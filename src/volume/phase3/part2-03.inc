// SPDX-License-Identifier: GPL-3.0-or-later
static int path_length_checked(const char *path, size_t *length_out)
{
    if (!path || !length_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    size_t length = 0;
    while (length < INFS_PATH_MAX && path[length] != '\0')
        ++length;
    if (length == INFS_PATH_MAX)
        return INFS_STATUS_NAME_TOO_LONG;
    *length_out = length;
    return INFS_STATUS_OK;
}

infs_status infs_lookup_path(struct infs_volume *vol, const char *path,
                             struct infs_lookup *out)
{
    if (!vol || !path || !out || path[0] != '/')
        return INFS_STATUS_INVALID_ARGUMENT;
    size_t path_length = 0;
    infs_status status = path_length_checked(path, &path_length);
    if (status != INFS_STATUS_OK)
        return status;
    (void)path_length;

    struct infs_lookup root = {
        .block = infs_le64_to_cpu(vol->sb.root_object_block),
        .type = INFS_OBJECT_DIRECTORY
    };
    memcpy(root.object_id, vol->sb.root_object_id, 16);
    return lookup_from(vol, &root, path, out);
}

static int split_parent(struct infs_volume *vol, const char *path,
                        struct infs_lookup *parent, char name[INFS_NAME_MAX + 1u])
{
    if (!vol || !path || !parent || !name || path[0] != '/' ||
        strcmp(path, "/") == 0)
        return INFS_STATUS_INVALID_ARGUMENT;

    size_t len = 0;
    infs_status status = path_length_checked(path, &len);
    if (status != INFS_STATUS_OK)
        return status;
    while (len > 1 && path[len - 1] == '/')
        --len;
    const char *slash = path + len;
    while (slash > path && slash[-1] != '/')
        --slash;
    size_t name_len = (size_t)((path + len) - slash);
    if (name_len == 0 || name_len > INFS_NAME_MAX)
        return INFS_STATUS_NAME_TOO_LONG;
    memcpy(name, slash, name_len);
    name[name_len] = '\0';
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return INFS_STATUS_INVALID_ARGUMENT;

    char parent_path[INFS_PATH_MAX];
    size_t plen = (size_t)(slash - path);
    if (plen == 0)
        plen = 1;
    if (plen >= sizeof(parent_path))
        return INFS_STATUS_NAME_TOO_LONG;
    memcpy(parent_path, path, plen);
    parent_path[plen] = '\0';
    status = infs_lookup_path(vol, parent_path, parent);
    if (status != INFS_STATUS_OK)
        return status;
    if (parent->type != INFS_OBJECT_DIRECTORY)
        return INFS_STATUS_NOT_DIRECTORY;
    return INFS_STATUS_OK;
}
