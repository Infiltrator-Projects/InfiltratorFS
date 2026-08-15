// SPDX-License-Identifier: GPL-3.0-or-later
static int command_cat(struct infs_volume *vol, const char *path)
{
    struct stat st;
    if (infs_getattr(vol, path, &st) != 0)
        return die_errno("cat");
    if (!S_ISREG(st.st_mode)) {
        errno = EISDIR;
        return die_errno("cat");
    }

    uint8_t buf[256 * 1024];
    off_t off = 0;
    while (off < st.st_size) {
        size_t want = sizeof(buf);
        if ((off_t)want > st.st_size - off)
            want = (size_t)(st.st_size - off);
        ssize_t n = infs_read_file(vol, path, buf, want, off);
        if (n < 0)
            return die_errno("read file");
        if (n == 0) {
            errno = EIO;
            return die_errno("short read");
        }
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

int main(int argc, char **argv)
{
    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }
    const char *image = argv[1];
    const char *cmd = argv[2];
    int writable = strcmp(cmd, "ls") != 0 && strcmp(cmd, "stat") != 0 &&
        strcmp(cmd, "cat") != 0 && strcmp(cmd, "map") != 0;

    struct infs_volume vol;
    if (infs_volume_open(&vol, image, writable) != 0)
        return die_errno("open InfiltratorFS");

    int rc = 0;
    if (strcmp(cmd, "ls") == 0) {
        if (argc > 4) rc = 2; else rc = command_ls(&vol, argc == 4 ? argv[3] : "/");
    } else if (strcmp(cmd, "stat") == 0 && argc == 4) {
        rc = command_stat(&vol, argv[3]);
    } else if (strcmp(cmd, "mkdir") == 0 && argc == 4) {
        if (infs_mkdir(&vol, argv[3], 0755, getuid(), getgid()) != 0) rc = die_errno("mkdir");
    } else if (strcmp(cmd, "put") == 0 && argc == 5) {
        rc = command_put(&vol, argv[3], argv[4]);
    } else if (strcmp(cmd, "write") == 0 && argc == 6) {
        rc = command_write(&vol, argv[3], argv[4], argv[5]);
    } else if (strcmp(cmd, "cat") == 0 && argc == 4) {
        rc = command_cat(&vol, argv[3]);
    } else if (strcmp(cmd, "map") == 0 && argc == 5) {
        rc = command_map(&vol, argv[3], argv[4]);
    } else if (strcmp(cmd, "mv") == 0 && argc == 5) {
        if (infs_rename(&vol, argv[3], argv[4]) != 0) rc = die_errno("mv");
    } else if (strcmp(cmd, "rm") == 0 && argc == 4) {
        if (infs_unlink(&vol, argv[3]) != 0) rc = die_errno("rm");
    } else if (strcmp(cmd, "rmdir") == 0 && argc == 4) {
        if (infs_rmdir(&vol, argv[3]) != 0) rc = die_errno("rmdir");
    } else {
        usage(argv[0]);
        rc = 2;
    }

    infs_volume_close(&vol);
    return rc;
}
