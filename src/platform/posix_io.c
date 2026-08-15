// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/posix_io.h"
#include "infilfs/volume.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int infs_pread_full(int fd, void *buf, size_t count, uint64_t offset)
{
    uint8_t *p = buf;
    while (count) {
        if (offset > (uint64_t)INT64_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
        ssize_t n = pread(fd, p, count, (off_t)offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        p += (size_t)n;
        count -= (size_t)n;
        offset += n;
    }
    return 0;
}

int infs_pwrite_full(int fd, const void *buf, size_t count, uint64_t offset)
{
    const uint8_t *p = buf;
    while (count) {
        if (offset > (uint64_t)INT64_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
        ssize_t n = pwrite(fd, p, count, (off_t)offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        p += (size_t)n;
        count -= (size_t)n;
        offset += n;
    }
    return 0;
}

int infs_get_size_bytes(int fd, uint64_t *size_bytes, int *is_block_device)
{
    struct stat st;
    if (fstat(fd, &st) != 0)
        return -1;

    *is_block_device = S_ISBLK(st.st_mode) ? 1 : 0;
    if (*is_block_device) {
        unsigned long long bytes = 0;
        if (ioctl(fd, BLKGETSIZE64, &bytes) != 0)
            return -1;
        *size_bytes = (uint64_t)bytes;
        return 0;
    }

    if (!S_ISREG(st.st_mode)) {
        errno = EINVAL;
        return -1;
    }

    *size_bytes = (uint64_t)st.st_size;
    return 0;
}

int infs_random_bytes(void *buf, size_t count)
{
    uint8_t *p = buf;
    while (count) {
        ssize_t n = getrandom(p, count, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)n;
        count -= (size_t)n;
    }
    return 0;
}

int64_t infs_current_time_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

struct infs_posix_storage_context {
    int fd;
};

static int posix_read_at(void *context, uint64_t offset, void *buffer, size_t size)
{
    struct infs_posix_storage_context *posix = context;
    return infs_pread_full(posix->fd, buffer, size, offset);
}

static int posix_write_at(void *context, uint64_t offset,
                          const void *buffer, size_t size)
{
    struct infs_posix_storage_context *posix = context;
    return infs_pwrite_full(posix->fd, buffer, size, offset);
}

static int posix_flush(void *context)
{
    struct infs_posix_storage_context *posix = context;
    return fsync(posix->fd);
}

static int posix_get_size(void *context, uint64_t *size_bytes, int *is_device)
{
    struct infs_posix_storage_context *posix = context;
    return infs_get_size_bytes(posix->fd, size_bytes, is_device);
}

static int posix_random(void *context, void *buffer, size_t size)
{
    (void)context;
    return infs_random_bytes(buffer, size);
}

static int64_t posix_time(void *context)
{
    (void)context;
    return infs_current_time_ns();
}

static void posix_close(void *context)
{
    struct infs_posix_storage_context *posix = context;
    if (posix->fd >= 0)
        close(posix->fd);
    free(posix);
}

static const struct infs_storage_ops posix_storage_ops = {
    .read_at = posix_read_at,
    .write_at = posix_write_at,
    .flush = posix_flush,
    .get_size = posix_get_size,
    .random_bytes = posix_random,
    .current_time_ns = posix_time,
    .close = posix_close,
};

int infs_posix_storage_open(struct infs_storage *storage,
                            const char *path, int writable)
{
    if (!storage || !path) {
        errno = EINVAL;
        return -1;
    }

    struct infs_posix_storage_context *context = malloc(sizeof(*context));
    if (!context)
        return -1;
    context->fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (context->fd < 0) {
        free(context);
        return -1;
    }

    storage->ops = &posix_storage_ops;
    storage->context = context;
    return 0;
}

int infs_posix_volume_open(struct infs_volume *vol, const char *path, int writable)
{
    struct infs_storage storage = {0};
    if (infs_posix_storage_open(&storage, path, writable) != 0)
        return -1;
    if (infs_volume_open_storage(vol, &storage, writable) != 0) {
        infs_storage_close(&storage);
        return -1;
    }
    return 0;
}
