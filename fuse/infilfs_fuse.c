// SPDX-License-Identifier: GPL-3.0-or-later
#define FUSE_USE_VERSION 35
#include <fuse3/fuse.h>

#include "infilfs/checksum.h"
#include "infilfs/format.h"
#include "infilfs/posix_io.h"
#include "infilfs/volume.h"

#include "infilfs/endian.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static struct infs_volume g_volume;

static int neg_errno(void)
{
    return errno ? -errno : -EIO;
}

static void attributes_to_stat(const struct infs_attributes *attributes,
                               struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = (attributes->object_type == INFS_OBJECT_DIRECTORY ?
                   S_IFDIR : S_IFREG) | attributes->posix_permissions;
    st->st_uid = attributes->posix_uid;
    st->st_gid = attributes->posix_gid;
    st->st_nlink = (nlink_t)attributes->link_count;
    st->st_size = (off_t)attributes->logical_size;
    st->st_blksize = INFS_BLOCK_SIZE;
    st->st_blocks = (blkcnt_t)(attributes->allocated_size / 512u);
    uint64_t inode = infs_crc64_ecma(attributes->object_id, 16);
    st->st_ino = (ino_t)(inode ? inode : 1u);

    int64_t at = attributes->access_time_ns;
    int64_t mt = attributes->modification_time_ns;
    int64_t ct = attributes->change_time_ns;
    st->st_atim.tv_sec = (time_t)(at / INT64_C(1000000000));
    st->st_atim.tv_nsec = (long)(at % INT64_C(1000000000));
    st->st_mtim.tv_sec = (time_t)(mt / INT64_C(1000000000));
    st->st_mtim.tv_nsec = (long)(mt % INT64_C(1000000000));
    st->st_ctim.tv_sec = (time_t)(ct / INT64_C(1000000000));
    st->st_ctim.tv_nsec = (long)(ct % INT64_C(1000000000));
}

static int infs_getattr_cb(const char *path, struct stat *st,
                           struct fuse_file_info *fi)
{
    (void)fi;
    struct infs_attributes attributes;
    if (infs_get_attributes(&g_volume, path, &attributes) != 0)
        return neg_errno();
    attributes_to_stat(&attributes, st);
    return 0;
}

static int infs_readdir_cb(const char *path, void *buf, fuse_fill_dir_t filler,
                           off_t off, struct fuse_file_info *fi,
                           enum fuse_readdir_flags flags)
{
    (void)off;
    (void)fi;
    (void)flags;
    if (filler(buf, ".", NULL, 0, 0) != 0 ||
        filler(buf, "..", NULL, 0, 0) != 0)
        return 0;

    struct infs_dir_item *items = NULL;
    size_t count = 0;
    if (infs_list_dir(&g_volume, path, &items, &count) != 0)
        return neg_errno();
    for (size_t i = 0; i < count; ++i) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_mode = items[i].type == INFS_OBJECT_DIRECTORY ? S_IFDIR : S_IFREG;
        if (filler(buf, items[i].name, &st, 0, 0) != 0)
            break;
    }
    infs_free_dir_items(items);
    return 0;
}

static int infs_mkdir_cb(const char *path, mode_t mode)
{
    struct fuse_context *ctx = fuse_get_context();
    const struct infs_create_options options = {
        .posix_permissions = (uint32_t)mode,
        .posix_uid = (uint32_t)ctx->uid,
        .posix_gid = (uint32_t)ctx->gid,
    };
    if (infs_mkdir(&g_volume, path, &options) != 0)
        return neg_errno();
    return 0;
}

static int infs_create_cb(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)fi;
    struct fuse_context *ctx = fuse_get_context();
    const struct infs_create_options options = {
        .posix_permissions = (uint32_t)mode,
        .posix_uid = (uint32_t)ctx->uid,
        .posix_gid = (uint32_t)ctx->gid,
    };
    if (infs_create_file(&g_volume, path, &options) != 0)
        return neg_errno();
    return 0;
}

static int infs_open_cb(const char *path, struct fuse_file_info *fi)
{
    struct infs_attributes attributes;
    if (infs_get_attributes(&g_volume, path, &attributes) != 0)
        return neg_errno();
    if (attributes.object_type != INFS_OBJECT_FILE)
        return -EISDIR;
    if ((fi->flags & O_TRUNC) && (fi->flags & O_ACCMODE) != O_RDONLY) {
        if (infs_truncate_file(&g_volume, path, 0) != 0)
            return neg_errno();
    }
    return 0;
}

static int infs_read_cb(const char *path, char *buf, size_t size, off_t off,
                        struct fuse_file_info *fi)
{
    (void)fi;
    if (off < 0)
        return -EINVAL;
    int64_t n = infs_read_file(&g_volume, path, buf, size, (uint64_t)off);
    if (n < 0)
        return neg_errno();
    return (int)n;
}

static int infs_write_cb(const char *path, const char *buf, size_t size, off_t off,
                         struct fuse_file_info *fi)
{
    if (fi && (fi->flags & O_APPEND)) {
        struct infs_attributes attributes;
        if (infs_get_attributes(&g_volume, path, &attributes) != 0)
            return neg_errno();
        off = (off_t)attributes.logical_size;
    }
    if (off < 0)
        return -EINVAL;
    int64_t n = infs_write_file(&g_volume, path, buf, size, (uint64_t)off);
    if (n < 0)
        return neg_errno();
    return (int)n;
}

static int infs_truncate_cb(const char *path, off_t size, struct fuse_file_info *fi)
{
    (void)fi;
    if (size < 0)
        return -EINVAL;
    if (infs_truncate_file(&g_volume, path, (uint64_t)size) != 0)
        return neg_errno();
    return 0;
}

static int infs_unlink_cb(const char *path)
{
    if (infs_unlink(&g_volume, path) != 0)
        return neg_errno();
    return 0;
}

static int infs_rmdir_cb(const char *path)
{
    if (infs_rmdir(&g_volume, path) != 0)
        return neg_errno();
    return 0;
}

static int infs_rename_cb(const char *oldpath, const char *newpath, unsigned flags)
{
    if (flags != 0)
        return -EOPNOTSUPP;
    if (infs_rename(&g_volume, oldpath, newpath) != 0)
        return neg_errno();
    return 0;
}

static int infs_chmod_cb(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)fi;
    if (infs_set_posix_compat(&g_volume, path, INFS_POSIX_SET_PERMISSIONS,
                              (uint32_t)mode, 0, 0) != 0)
        return neg_errno();
    return 0;
}

static int infs_chown_cb(const char *path, uid_t uid, gid_t gid,
                         struct fuse_file_info *fi)
{
    (void)fi;
    uint32_t mask = 0;
    if (uid != (uid_t)-1)
        mask |= INFS_POSIX_SET_UID;
    if (gid != (gid_t)-1)
        mask |= INFS_POSIX_SET_GID;
    if (infs_set_posix_compat(&g_volume, path, mask, 0,
                              (uint32_t)uid, (uint32_t)gid) != 0)
        return neg_errno();
    return 0;
}

static int infs_utimens_cb(const char *path, const struct timespec tv[2],
                           struct fuse_file_info *fi)
{
    (void)fi;
    struct infs_time_update update = {
        .access_action = tv[0].tv_nsec == UTIME_OMIT ? INFS_TIME_OMIT :
                         tv[0].tv_nsec == UTIME_NOW ? INFS_TIME_NOW : INFS_TIME_SET,
        .modification_action = tv[1].tv_nsec == UTIME_OMIT ? INFS_TIME_OMIT :
                               tv[1].tv_nsec == UTIME_NOW ? INFS_TIME_NOW : INFS_TIME_SET,
        .access_time_ns = (int64_t)tv[0].tv_sec * INT64_C(1000000000) + tv[0].tv_nsec,
        .modification_time_ns = (int64_t)tv[1].tv_sec * INT64_C(1000000000) + tv[1].tv_nsec,
    };
    if (infs_set_times(&g_volume, path, &update) != 0)
        return neg_errno();
    return 0;
}

static int infs_statfs_cb(const char *path, struct statvfs *st)
{
    (void)path;
    memset(st, 0, sizeof(*st));
    st->f_bsize = INFS_BLOCK_SIZE;
    st->f_frsize = INFS_BLOCK_SIZE;
    st->f_blocks = infs_le64_to_cpu(g_volume.sb.total_blocks);
    st->f_bfree = infs_le64_to_cpu(g_volume.sb.free_blocks);
    st->f_bavail = st->f_bfree;
    st->f_namemax = INFS_NAME_MAX;
    return 0;
}

static int infs_fsync_cb(const char *path, int datasync, struct fuse_file_info *fi)
{
    (void)path;
    (void)datasync;
    (void)fi;
    if (infs_volume_sync(&g_volume) != 0)
        return neg_errno();
    return 0;
}


static void *infs_init_cb(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)cfg;
#ifdef FUSE_CAP_HANDLE_KILLPRIV
    conn->want &= ~FUSE_CAP_HANDLE_KILLPRIV;
#endif
#ifdef FUSE_CAP_HANDLE_KILLPRIV_V2
    conn->want &= ~FUSE_CAP_HANDLE_KILLPRIV_V2;
#endif
    return NULL;
}

static const struct fuse_operations infs_ops = {
    .init = infs_init_cb,
    .getattr = infs_getattr_cb,
    .readdir = infs_readdir_cb,
    .mkdir = infs_mkdir_cb,
    .create = infs_create_cb,
    .open = infs_open_cb,
    .read = infs_read_cb,
    .write = infs_write_cb,
    .truncate = infs_truncate_cb,
    .unlink = infs_unlink_cb,
    .rmdir = infs_rmdir_cb,
    .rename = infs_rename_cb,
    .chmod = infs_chmod_cb,
    .chown = infs_chown_cb,
    .utimens = infs_utimens_cb,
    .statfs = infs_statfs_cb,
    .fsync = infs_fsync_cb,
};

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <image-or-device> <mountpoint> [FUSE options]\n",
                argv[0]);
        return 2;
    }

    if (infs_posix_volume_open(&g_volume, argv[1], 1) != 0) {
        fprintf(stderr, "Cannot mount InfiltratorFS: %s\n", strerror(errno));
        return 1;
    }

    char **fuse_argv = calloc((size_t)argc + 3u, sizeof(*fuse_argv));
    if (!fuse_argv) {
        perror("calloc");
        infs_volume_close(&g_volume);
        return 1;
    }

    int j = 0;
    fuse_argv[j++] = argv[0];
    for (int i = 2; i < argc; ++i)
        fuse_argv[j++] = argv[i];
    fuse_argv[j++] = "-s"; /* The current core remains deliberately single-writer. */
    fuse_argv[j++] = "-o";
    fuse_argv[j++] = "default_permissions";

    int rc = fuse_main(j, fuse_argv, &infs_ops, NULL);
    free(fuse_argv);
    infs_volume_close(&g_volume);
    return rc;
}
