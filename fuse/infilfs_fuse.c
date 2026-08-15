// SPDX-License-Identifier: GPL-3.0-or-later
#define FUSE_USE_VERSION 35
#include <fuse3/fuse.h>

#include "infilfs/format.h"
#include "infilfs/fs.h"
#include "infilfs/io.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

struct infs_mount_state {
    int fd;
    struct infs_superblock_disk sb;
};

static struct infs_mount_state g_state = { .fd = -1 };

static int infs_getattr_cb(const char *path, struct stat *st,
                           struct fuse_file_info *fi)
{
    (void)fi;
    memset(st, 0, sizeof(*st));
    if (strcmp(path, "/") != 0)
        return -ENOENT;

    st->st_mode = S_IFDIR | 0755;
    st->st_nlink = 2;
    st->st_blksize = INFS_BLOCK_SIZE;
    return 0;
}

static int infs_readdir_cb(const char *path, void *buf, fuse_fill_dir_t filler,
                           off_t off, struct fuse_file_info *fi,
                           enum fuse_readdir_flags flags)
{
    (void)off;
    (void)fi;
    (void)flags;
    if (strcmp(path, "/") != 0)
        return -ENOENT;

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    return 0;
}

static int infs_statfs_cb(const char *path, struct statvfs *st)
{
    (void)path;
    memset(st, 0, sizeof(*st));
    st->f_bsize = INFS_BLOCK_SIZE;
    st->f_frsize = INFS_BLOCK_SIZE;
    st->f_blocks = le64toh(g_state.sb.total_blocks);
    st->f_bfree = le64toh(g_state.sb.free_blocks);
    st->f_bavail = st->f_bfree;
    st->f_namemax = 255;
    return 0;
}

static const struct fuse_operations infs_ops = {
    .getattr = infs_getattr_cb,
    .readdir = infs_readdir_cb,
    .statfs = infs_statfs_cb,
};

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <image-or-device> <mountpoint> [FUSE options]\n",
                argv[0]);
        return 2;
    }

    g_state.fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (g_state.fd < 0) {
        perror("open");
        return 1;
    }

    uint64_t size_bytes = 0;
    int is_block = 0;
    unsigned valid = 0;
    if (infs_get_size_bytes(g_state.fd, &size_bytes, &is_block) != 0 ||
        infs_read_best_superblock(g_state.fd, size_bytes, &g_state.sb, &valid) != 0) {
        fprintf(stderr, "Cannot mount: no valid InfiltratorFS checkpoint.\n");
        close(g_state.fd);
        return 1;
    }
    (void)is_block;

    uint8_t root[INFS_BLOCK_SIZE];
    uint64_t root_no = le64toh(g_state.sb.root_object_block);
    if (infs_pread_full(g_state.fd, root, sizeof(root),
                        (off_t)(root_no * INFS_BLOCK_SIZE)) != 0 ||
        !infs_validate_object_block(root)) {
        fprintf(stderr, "Cannot mount: root metadata object is invalid.\n");
        close(g_state.fd);
        return 1;
    }

    char **fuse_argv = calloc((size_t)argc, sizeof(*fuse_argv));
    if (!fuse_argv) {
        perror("calloc");
        close(g_state.fd);
        return 1;
    }

    fuse_argv[0] = argv[0];
    for (int i = 2; i < argc; ++i)
        fuse_argv[i - 1] = argv[i];
    int fuse_argc = argc - 1;

    int rc = fuse_main(fuse_argc, fuse_argv, &infs_ops, NULL);
    free(fuse_argv);
    close(g_state.fd);
    return rc;
}
