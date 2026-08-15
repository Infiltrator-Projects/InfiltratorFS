// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/volume.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s <image> ls [path]\n"
        "  %s <image> stat <path>\n"
        "  %s <image> mkdir <path>\n"
        "  %s <image> put <host-file> <path>\n"
        "  %s <image> cat <path>\n"
        "  %s <image> mv <old-path> <new-path>\n"
        "  %s <image> rm <path>\n"
        "  %s <image> rmdir <path>\n",
        prog, prog, prog, prog, prog, prog, prog, prog);
}

static int die_errno(const char *what)
{
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
    return 1;
}

static int command_ls(struct infs_volume *vol, const char *path)
{
    struct infs_dir_item *items = NULL;
    size_t count = 0;
    if (infs_list_dir(vol, path, &items, &count) != 0)
        return die_errno("ls");
    for (size_t i = 0; i < count; ++i)
        printf("%c %s\n", items[i].type == INFS_OBJECT_DIRECTORY ? 'd' : '-', items[i].name);
    infs_free_dir_items(items);
    return 0;
}

static int command_stat(struct infs_volume *vol, const char *path)
{
    struct stat st;
    if (infs_getattr(vol, path, &st) != 0)
        return die_errno("stat");
    printf("mode=%#o uid=%u gid=%u nlink=%ju size=%jd blocks=%jd\n",
           (unsigned)st.st_mode, (unsigned)st.st_uid, (unsigned)st.st_gid,
           (uintmax_t)st.st_nlink, (intmax_t)st.st_size, (intmax_t)st.st_blocks);
    return 0;
}

static int command_put(struct infs_volume *vol, const char *host, const char *dest)
{
    int input = open(host, O_RDONLY | O_CLOEXEC);
    if (input < 0)
        return die_errno("open host file");

    struct stat st;
    if (infs_getattr(vol, dest, &st) != 0) {
        if (errno != ENOENT) {
            close(input);
            return die_errno("lookup destination");
        }
        if (infs_create_file(vol, dest, 0644, getuid(), getgid()) != 0) {
            close(input);
            return die_errno("create destination");
        }
    } else if (!S_ISREG(st.st_mode)) {
        close(input);
        errno = EISDIR;
        return die_errno("destination");
    }

    if (infs_truncate_file(vol, dest, 0) != 0) {
        close(input);
        return die_errno("truncate destination");
    }

    uint8_t buf[256 * 1024];
    off_t off = 0;
    for (;;) {
        ssize_t n = read(input, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(input);
            return die_errno("read host file");
        }
        if (n == 0)
            break;
        ssize_t w = infs_write_file(vol, dest, buf, (size_t)n, off);
        if (w < 0 || w != n) {
            close(input);
            if (w >= 0)
                errno = EIO;
            return die_errno("write destination");
        }
        off += w;
    }
    close(input);
    return 0;
}

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
    int writable = strcmp(cmd, "ls") != 0 && strcmp(cmd, "stat") != 0 && strcmp(cmd, "cat") != 0;

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
    } else if (strcmp(cmd, "cat") == 0 && argc == 4) {
        rc = command_cat(&vol, argv[3]);
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
