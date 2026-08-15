// SPDX-License-Identifier: GPL-3.0-or-later
int infs_lookup_path(struct infs_volume *vol, const char *path,
                     struct infs_lookup *out)
{
    if (!vol || !path || !out || path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
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
    if (!path || path[0] != '/' || strcmp(path, "/") == 0) {
        errno = EINVAL;
        return -1;
    }
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        --len;
    const char *slash = path + len;
    while (slash > path && slash[-1] != '/')
        --slash;
    size_t name_len = (size_t)((path + len) - slash);
    if (name_len == 0 || name_len > INFS_NAME_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(name, slash, name_len);
    name[name_len] = '\0';
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        errno = EINVAL;
        return -1;
    }

    char parent_path[INFS_PATH_MAX];
    size_t plen = (size_t)(slash - path);
    if (plen == 0)
        plen = 1;
    if (plen >= sizeof(parent_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(parent_path, path, plen);
    parent_path[plen] = '\0';
    if (infs_lookup_path(vol, parent_path, parent) != 0)
        return -1;
    if (parent->type != INFS_OBJECT_DIRECTORY) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}
