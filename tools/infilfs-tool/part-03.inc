// SPDX-License-Identifier: GPL-3.0-or-later
static int command_cat(struct infs_volume *vol, const char *path)
{
    struct infs_attributes attributes;
    infs_status status = infs_get_attributes(vol, path, &attributes);
    if (status != INFS_STATUS_OK)
        return die_status("cat", status);
    if (attributes.object_type != INFS_OBJECT_FILE)
        return die_status("cat", INFS_STATUS_IS_DIRECTORY);

    uint8_t buf[256 * 1024];
    uint64_t off = 0;
    while (off < attributes.logical_size) {
        size_t want = sizeof(buf);
        if ((uint64_t)want > attributes.logical_size - off)
            want = (size_t)(attributes.logical_size - off);
        int64_t n = infs_read_file(vol, path, buf, want, off);
        if (n < 0)
            return die_status("read file", (infs_status)n);
        if (n == 0)
            return die_status("short read", INFS_STATUS_IO_ERROR);
        size_t done = 0;
        while (done < (size_t)n) {
            ssize_t w = write(STDOUT_FILENO, buf + done, (size_t)n - done);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                return die_errno("stdout");
            }
            done += (size_t)w;
        }
        off += n;
    }
    return 0;
}

static int command_readlink(struct infs_volume *vol, const char *path)
{
    char target[INFS_SYMLINK_TARGET_MAX + 1u];
    size_t length = 0;
    infs_status status = infs_read_symlink(
        vol, path, target, sizeof(target), &length);
    if (status != INFS_STATUS_OK)
        return die_status("readlink", status);
    if (fwrite(target, 1, length, stdout) != length || putchar('\n') == EOF)
        return die_errno("stdout");
    return 0;
}

static int command_snapshot_list(struct infs_volume *vol)
{
    struct infs_snapshot_info *snapshots = NULL;
    size_t count = 0;
    infs_status status = infs_snapshot_list(vol, &snapshots, &count);
    if (status != INFS_STATUS_OK)
        return die_status("snapshot-list", status);
    for (size_t i = 0; i < count; ++i)
        printf("%s generation=%" PRIu64 " created_ns=%" PRId64 "\n",
               snapshots[i].name, snapshots[i].generation,
               snapshots[i].created_time_ns);
    infs_free_snapshot_infos(snapshots);
    return 0;
}

static int command_snapshot_ls(struct infs_volume *vol, const char *snapshot,
                               const char *path)
{
    struct infs_dir_item *items = NULL;
    size_t count = 0;
    infs_status status = infs_snapshot_list_dir(
        vol, snapshot, path, &items, &count);
    if (status != INFS_STATUS_OK)
        return die_status("snapshot-ls", status);
    for (size_t i = 0; i < count; ++i)
        printf("%c %s\n",
               items[i].type == INFS_OBJECT_DIRECTORY ? 'd' :
               items[i].type == INFS_OBJECT_SYMLINK ? 'l' : '-',
               items[i].name);
    infs_free_dir_items(items);
    return 0;
}

static int command_snapshot_stat(struct infs_volume *vol,
                                 const char *snapshot, const char *path)
{
    struct infs_attributes attributes;
    infs_status status = infs_snapshot_get_attributes(
        vol, snapshot, path, &attributes);
    if (status != INFS_STATUS_OK)
        return die_status("snapshot-stat", status);
    printf("type=%u mode=%#o uid=%u gid=%u links=%" PRIu64
           " size=%" PRIu64 " allocated=%" PRIu64 "\n",
           attributes.object_type, attributes.posix_permissions,
           attributes.posix_uid, attributes.posix_gid,
           attributes.link_count, attributes.logical_size,
           attributes.allocated_size);
    return 0;
}

static int command_snapshot_cat(struct infs_volume *vol,
                                const char *snapshot, const char *path)
{
    struct infs_attributes attributes;
    infs_status status = infs_snapshot_get_attributes(
        vol, snapshot, path, &attributes);
    if (status != INFS_STATUS_OK)
        return die_status("snapshot-cat", status);
    if (attributes.object_type != INFS_OBJECT_FILE)
        return die_status("snapshot-cat", INFS_STATUS_IS_DIRECTORY);
    uint8_t buf[256 * 1024];
    uint64_t off = 0;
    while (off < attributes.logical_size) {
        size_t want = sizeof(buf);
        if ((uint64_t)want > attributes.logical_size - off)
            want = (size_t)(attributes.logical_size - off);
        int64_t n = infs_snapshot_read_file(
            vol, snapshot, path, buf, want, off);
        if (n <= 0)
            return die_status("snapshot-cat",
                              n < 0 ? (infs_status)n : INFS_STATUS_IO_ERROR);
        if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n)
            return die_errno("stdout");
        off += (uint64_t)n;
    }
    return 0;
}

static int command_snapshot_readlink(struct infs_volume *vol,
                                     const char *snapshot, const char *path)
{
    char target[INFS_SYMLINK_TARGET_MAX + 1u];
    size_t length = 0;
    infs_status status = infs_snapshot_read_symlink(
        vol, snapshot, path, target, sizeof(target), &length);
    if (status != INFS_STATUS_OK)
        return die_status("snapshot-readlink", status);
    if (fwrite(target, 1, length, stdout) != length || putchar('\n') == EOF)
        return die_errno("stdout");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }
    const char *image = argv[1];
    const char *cmd = argv[2];
    int writable = strcmp(cmd, "ls") != 0 && strcmp(cmd, "stat") != 0 &&
        strcmp(cmd, "cat") != 0 && strcmp(cmd, "map") != 0 &&
        strcmp(cmd, "readlink") != 0 && strcmp(cmd, "snapshot-list") != 0 &&
        strcmp(cmd, "snapshot-ls") != 0 &&
        strcmp(cmd, "snapshot-stat") != 0 &&
        strcmp(cmd, "snapshot-cat") != 0 &&
        strcmp(cmd, "snapshot-readlink") != 0;

    struct infs_volume vol;
    infs_status status = infs_posix_volume_open(&vol, image, writable);
    if (status != INFS_STATUS_OK)
        return die_status("open InfiltratorFS", status);

    int rc = 0;
    if (strcmp(cmd, "ls") == 0) {
        if (argc > 4) rc = 2; else rc = command_ls(&vol, argc == 4 ? argv[3] : "/");
    } else if (strcmp(cmd, "stat") == 0 && argc == 4) {
        rc = command_stat(&vol, argv[3]);
    } else if (strcmp(cmd, "mkdir") == 0 && argc == 4) {
        const struct infs_create_options options = {
            .posix_permissions = 0755,
            .posix_uid = (uint32_t)getuid(),
            .posix_gid = (uint32_t)getgid(),
        };
        status = infs_mkdir(&vol, argv[3], &options);
        if (status != INFS_STATUS_OK) rc = die_status("mkdir", status);
    } else if (strcmp(cmd, "put") == 0 && argc == 5) {
        rc = command_put(&vol, argv[3], argv[4]);
    } else if (strcmp(cmd, "write") == 0 && argc == 6) {
        rc = command_write(&vol, argv[3], argv[4], argv[5]);
    } else if (strcmp(cmd, "punch") == 0 && argc == 6) {
        rc = command_punch(&vol, argv[3], argv[4], argv[5]);
    } else if (strcmp(cmd, "cat") == 0 && argc == 4) {
        rc = command_cat(&vol, argv[3]);
    } else if (strcmp(cmd, "map") == 0 && argc == 5) {
        rc = command_map(&vol, argv[3], argv[4]);
    } else if (strcmp(cmd, "reflink") == 0 && argc == 5) {
        status = infs_reflink_file(&vol, argv[3], argv[4]);
        if (status != INFS_STATUS_OK) rc = die_status("reflink", status);
    } else if (strcmp(cmd, "symlink") == 0 && argc == 5) {
        const struct infs_create_options options = {
            .posix_permissions = 0777,
            .posix_uid = (uint32_t)getuid(),
            .posix_gid = (uint32_t)getgid(),
        };
        status = infs_create_symlink(&vol, argv[4], argv[3], &options);
        if (status != INFS_STATUS_OK) rc = die_status("symlink", status);
    } else if (strcmp(cmd, "readlink") == 0 && argc == 4) {
        rc = command_readlink(&vol, argv[3]);
    } else if (strcmp(cmd, "link") == 0 && argc == 5) {
        status = infs_link_file(&vol, argv[3], argv[4]);
        if (status != INFS_STATUS_OK) rc = die_status("link", status);
    } else if (strcmp(cmd, "mv") == 0 && argc == 5) {
        status = infs_rename(&vol, argv[3], argv[4]);
        if (status != INFS_STATUS_OK) rc = die_status("mv", status);
    } else if (strcmp(cmd, "rm") == 0 && argc == 4) {
        status = infs_unlink(&vol, argv[3]);
        if (status != INFS_STATUS_OK) rc = die_status("rm", status);
    } else if (strcmp(cmd, "rmdir") == 0 && argc == 4) {
        status = infs_rmdir(&vol, argv[3]);
        if (status != INFS_STATUS_OK) rc = die_status("rmdir", status);
    } else if (strcmp(cmd, "snapshot-create") == 0 && argc == 4) {
        status = infs_snapshot_create(&vol, argv[3]);
        if (status != INFS_STATUS_OK) rc = die_status("snapshot-create", status);
    } else if (strcmp(cmd, "snapshot-list") == 0 && argc == 3) {
        rc = command_snapshot_list(&vol);
    } else if (strcmp(cmd, "snapshot-delete") == 0 && argc == 4) {
        status = infs_snapshot_delete(&vol, argv[3]);
        if (status != INFS_STATUS_OK) rc = die_status("snapshot-delete", status);
    } else if (strcmp(cmd, "snapshot-ls") == 0 &&
               (argc == 4 || argc == 5)) {
        rc = command_snapshot_ls(&vol, argv[3], argc == 5 ? argv[4] : "/");
    } else if (strcmp(cmd, "snapshot-stat") == 0 && argc == 5) {
        rc = command_snapshot_stat(&vol, argv[3], argv[4]);
    } else if (strcmp(cmd, "snapshot-cat") == 0 && argc == 5) {
        rc = command_snapshot_cat(&vol, argv[3], argv[4]);
    } else if (strcmp(cmd, "snapshot-readlink") == 0 && argc == 5) {
        rc = command_snapshot_readlink(&vol, argv[3], argv[4]);
    } else {
        usage(argv[0]);
        rc = 2;
    }

    infs_volume_close(&vol);
    return rc;
}
