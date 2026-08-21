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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE 0x01
#endif
#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE 0x02
#endif

#define INFS_NS_PER_SECOND INT64_C(1000000000)

static struct infs_volume g_volume;

static int neg_status(infs_status status)
{
    return -infs_status_to_errno(status);
}

static void apply_mount_options(const char *options, int *read_only)
{
    const char *p = options;
    while (p && *p) {
        const char *end = strchr(p, ',');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        if (length == 2u && memcmp(p, "ro", 2) == 0)
            *read_only = 1;
        else if (length == 2u && memcmp(p, "rw", 2) == 0)
            *read_only = 0;
        if (!end)
            break;
        p = end + 1;
    }
}

static int fuse_requested_read_only(int argc, char **argv)
{
    int read_only = 0;
    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "-r") == 0 ||
            strcmp(argv[i], "--read-only") == 0) {
            read_only = 1;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            apply_mount_options(argv[++i], &read_only);
        } else if (strncmp(argv[i], "-o", 2) == 0 && argv[i][2] != '\0') {
            apply_mount_options(argv[i] + 2, &read_only);
        }
    }
    return read_only;
}

static int uint64_to_off_t(uint64_t value, off_t *out)
{
    if (!out)
        return -EINVAL;
    off_t converted = (off_t)value;
    if (converted < 0 || (uint64_t)converted != value)
        return -EOVERFLOW;
    *out = converted;
    return 0;
}

static void ns_to_timespec(int64_t ns, struct timespec *out)
{
    int64_t seconds = ns / INFS_NS_PER_SECOND;
    int64_t nanoseconds = ns % INFS_NS_PER_SECOND;
    if (nanoseconds < 0) {
        --seconds;
        nanoseconds += INFS_NS_PER_SECOND;
    }
    out->tv_sec = (time_t)seconds;
    out->tv_nsec = (long)nanoseconds;
}

static int timespec_to_ns(const struct timespec *value, int64_t *out)
{
    if (!value || !out || value->tv_nsec < 0 ||
        value->tv_nsec >= INFS_NS_PER_SECOND)
        return -EINVAL;

    int64_t seconds = (int64_t)value->tv_sec;
    if ((time_t)seconds != value->tv_sec)
        return -EOVERFLOW;
    if (seconds > INT64_MAX / INFS_NS_PER_SECOND ||
        seconds < INT64_MIN / INFS_NS_PER_SECOND)
        return -EOVERFLOW;

    int64_t base = seconds * INFS_NS_PER_SECOND;
    if (base > INT64_MAX - (int64_t)value->tv_nsec)
        return -EOVERFLOW;
    *out = base + (int64_t)value->tv_nsec;
    return 0;
}

static int attributes_to_stat(const struct infs_attributes *attributes,
                              struct stat *st)
{
    if (!attributes || !st)
        return -EINVAL;
    off_t logical_size = 0;
    int rc = uint64_to_off_t(attributes->logical_size, &logical_size);
    if (rc != 0)
        return rc;

    memset(st, 0, sizeof(*st));
    st->st_mode = (attributes->object_type == INFS_OBJECT_DIRECTORY ?
                   S_IFDIR : S_IFREG) | attributes->posix_permissions;
    st->st_uid = (uid_t)attributes->posix_uid;
    st->st_gid = (gid_t)attributes->posix_gid;
    st->st_nlink = (nlink_t)attributes->link_count;
    st->st_size = logical_size;
    st->st_blksize = INFS_BLOCK_SIZE;
    st->st_blocks = (blkcnt_t)(attributes->allocated_size / 512u);
    uint64_t inode = infs_crc64_ecma(attributes->object_id, 16);
    st->st_ino = (ino_t)(inode ? inode : 1u);

    ns_to_timespec(attributes->access_time_ns, &st->st_atim);
    ns_to_timespec(attributes->modification_time_ns, &st->st_mtim);
    ns_to_timespec(attributes->change_time_ns, &st->st_ctim);
    return 0;
}

static int infs_getattr_cb(const char *path, struct stat *st,
                           struct fuse_file_info *fi)
{
    (void)fi;
    struct infs_attributes attributes;
    infs_status status = infs_get_attributes(&g_volume, path, &attributes);
    if (status != INFS_STATUS_OK)
        return neg_status(status);
    return attributes_to_stat(&attributes, st);
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
    infs_status status = infs_list_dir(&g_volume, path, &items, &count);
    if (status != INFS_STATUS_OK)
        return neg_status(status);
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
    return neg_status(infs_mkdir(&g_volume, path, &options));
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
    return neg_status(infs_create_file(&g_volume, path, &options));
}

static int infs_open_cb(const char *path, struct fuse_file_info *fi)
{
    struct infs_attributes attributes;
    infs_status status = infs_get_attributes(&g_volume, path, &attributes);
    if (status != INFS_STATUS_OK)
        return neg_status(status);
    if (attributes.object_type != INFS_OBJECT_FILE)
        return -EISDIR;
    if ((fi->flags & O_TRUNC) && (fi->flags & O_ACCMODE) != O_RDONLY) {
        status = infs_truncate_file(&g_volume, path, 0);
        if (status != INFS_STATUS_OK)
            return neg_status(status);
    }
    return 0;
}

static int infs_read_cb(const char *path, char *buf, size_t size, off_t off,
                        struct fuse_file_info *fi)
{
    (void)fi;
    if (off < 0)
        return -EINVAL;
    if (size > (size_t)INT_MAX)
        return -EOVERFLOW;
    int64_t n = infs_read_file(&g_volume, path, buf, size, (uint64_t)off);
    if (n < 0)
        return neg_status((infs_status)n);
    return (int)n;
}

static int infs_write_cb(const char *path, const char *buf, size_t size, off_t off,
                         struct fuse_file_info *fi)
{
    if (off < 0)
        return -EINVAL;
    if (size > (size_t)INT_MAX)
        return -EOVERFLOW;
    if (fi && (fi->flags & O_APPEND)) {
        struct infs_attributes attributes;
        infs_status status = infs_get_attributes(&g_volume, path, &attributes);
        if (status != INFS_STATUS_OK)
            return neg_status(status);
        int rc = uint64_to_off_t(attributes.logical_size, &off);
        if (rc != 0)
            return rc;
    }
    int64_t n = infs_write_file_buffered(&g_volume, path, buf, size,
                                         (uint64_t)off);
    if (n < 0)
        return neg_status((infs_status)n);
    return (int)n;
}

static int infs_truncate_cb(const char *path, off_t size, struct fuse_file_info *fi)
{
    (void)fi;
    if (size < 0)
        return -EINVAL;
    return neg_status(infs_truncate_file(&g_volume, path, (uint64_t)size));
}

static int infs_fallocate_cb(const char *path, int mode, off_t offset,
                             off_t length, struct fuse_file_info *fi)
{
    (void)fi;
    if (offset < 0 || length <= 0)
        return -EINVAL;
    if (mode != (FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE))
        return -EOPNOTSUPP;
    return neg_status(infs_punch_hole(&g_volume, path, (uint64_t)offset,
                                     (uint64_t)length));
}

static int infs_unlink_cb(const char *path)
{
    return neg_status(infs_unlink(&g_volume, path));
}

static int infs_rmdir_cb(const char *path)
{
    return neg_status(infs_rmdir(&g_volume, path));
}

static int infs_rename_cb(const char *oldpath, const char *newpath, unsigned flags)
{
    if (flags != 0)
        return -EOPNOTSUPP;
    return neg_status(infs_rename(&g_volume, oldpath, newpath));
}

static int infs_chmod_cb(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)fi;
    return neg_status(infs_set_posix_compat(
        &g_volume, path, INFS_POSIX_SET_PERMISSIONS,
        (uint32_t)mode, 0, 0));
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
    return neg_status(infs_set_posix_compat(
        &g_volume, path, mask, 0, (uint32_t)uid, (uint32_t)gid));
}

static int infs_utimens_cb(const char *path, const struct timespec tv[2],
                           struct fuse_file_info *fi)
{
    (void)fi;
    if (!tv)
        return neg_status(infs_set_times(&g_volume, path, NULL));

    struct infs_time_update update = {
        .access_action = tv[0].tv_nsec == UTIME_OMIT ? INFS_TIME_OMIT :
                         tv[0].tv_nsec == UTIME_NOW ? INFS_TIME_NOW : INFS_TIME_SET,
        .modification_action = tv[1].tv_nsec == UTIME_OMIT ? INFS_TIME_OMIT :
                               tv[1].tv_nsec == UTIME_NOW ? INFS_TIME_NOW : INFS_TIME_SET,
    };
    if (update.access_action == INFS_TIME_SET) {
        int rc = timespec_to_ns(&tv[0], &update.access_time_ns);
        if (rc != 0)
            return rc;
    }
    if (update.modification_action == INFS_TIME_SET) {
        int rc = timespec_to_ns(&tv[1], &update.modification_time_ns);
        if (rc != 0)
            return rc;
    }
    return neg_status(infs_set_times(&g_volume, path, &update));
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

static int sync_pending_writes(void)
{
    if (!g_volume.writable || !g_volume.tx_active)
        return 0;
    return neg_status(infs_volume_sync(&g_volume));
}

static int infs_flush_cb(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    return sync_pending_writes();
}

static int infs_release_cb(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    return sync_pending_writes();
}

static int infs_fsync_cb(const char *path, int datasync, struct fuse_file_info *fi)
{
    (void)path;
    (void)datasync;
    (void)fi;
    return sync_pending_writes();
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
    .flush = infs_flush_cb,
    .release = infs_release_cb,
    .truncate = infs_truncate_cb,
    .fallocate = infs_fallocate_cb,
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

    int read_only = fuse_requested_read_only(argc, argv);
    infs_status status = infs_posix_volume_open(&g_volume, argv[1], !read_only);
    if (status != INFS_STATUS_OK) {
        fprintf(stderr, "Cannot mount InfiltratorFS%s: %s\n",
                read_only ? " read-only" : "",
                infs_status_string(status));
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
    if (g_volume.writable && g_volume.tx_active) {
        infs_status sync_status = infs_volume_sync(&g_volume);
        if (sync_status != INFS_STATUS_OK && rc == 0) {
            fprintf(stderr, "Final InfiltratorFS sync failed: %s\n",
                    infs_status_string(sync_status));
            rc = 1;
        }
    }
    infs_volume_close(&g_volume);
    return rc;
}
