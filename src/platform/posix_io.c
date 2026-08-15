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

infs_status infs_status_from_errno(int error_number)
{
    switch (error_number) {
    case 0: return INFS_STATUS_OK;
    case EINVAL: return INFS_STATUS_INVALID_ARGUMENT;
    case EIO: return INFS_STATUS_IO_ERROR;
    case EROFS: return INFS_STATUS_READ_ONLY;
    case ENOSPC: return INFS_STATUS_NO_SPACE;
    case ENOENT: return INFS_STATUS_NOT_FOUND;
    case EEXIST: return INFS_STATUS_ALREADY_EXISTS;
    case ENOTDIR: return INFS_STATUS_NOT_DIRECTORY;
    case EISDIR: return INFS_STATUS_IS_DIRECTORY;
    case ENOTEMPTY: return INFS_STATUS_NOT_EMPTY;
    case ENAMETOOLONG: return INFS_STATUS_NAME_TOO_LONG;
    case EFBIG: return INFS_STATUS_FILE_TOO_LARGE;
    case ENOMEM: return INFS_STATUS_NO_MEMORY;
    case EINTR: return INFS_STATUS_INTERRUPTED;
#ifdef EOVERFLOW
    case EOVERFLOW: return INFS_STATUS_OVERFLOW;
#endif
#ifdef ELOOP
    case ELOOP: return INFS_STATUS_LOOP_DETECTED;
#endif
#ifdef EOPNOTSUPP
    case EOPNOTSUPP: return INFS_STATUS_NOT_SUPPORTED;
#endif
    default: return INFS_STATUS_ERROR;
    }
}

int infs_status_to_errno(infs_status status)
{
    switch (status) {
    case INFS_STATUS_OK: return 0;
    case INFS_STATUS_INVALID_ARGUMENT: return EINVAL;
    case INFS_STATUS_IO_ERROR:
    case INFS_STATUS_CORRUPT: return EIO;
    case INFS_STATUS_READ_ONLY: return EROFS;
    case INFS_STATUS_NO_SPACE: return ENOSPC;
    case INFS_STATUS_NOT_FOUND: return ENOENT;
    case INFS_STATUS_ALREADY_EXISTS: return EEXIST;
    case INFS_STATUS_NOT_DIRECTORY: return ENOTDIR;
    case INFS_STATUS_IS_DIRECTORY: return EISDIR;
    case INFS_STATUS_NOT_EMPTY: return ENOTEMPTY;
    case INFS_STATUS_NAME_TOO_LONG: return ENAMETOOLONG;
    case INFS_STATUS_FILE_TOO_LARGE: return EFBIG;
    case INFS_STATUS_NO_MEMORY: return ENOMEM;
    case INFS_STATUS_INTERRUPTED: return EINTR;
    case INFS_STATUS_OVERFLOW: return EOVERFLOW;
    case INFS_STATUS_LOOP_DETECTED: return ELOOP;
    case INFS_STATUS_NOT_SUPPORTED: return EOPNOTSUPP;
    default: return EIO;
    }
}

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

static infs_status posix_read_at(void *context, uint64_t offset,
                                 void *buffer, size_t size)
{
    struct infs_posix_storage_context *posix = context;
    if (infs_pread_full(posix->fd, buffer, size, offset) != 0)
        return infs_status_from_errno(errno);
    return INFS_STATUS_OK;
}

static infs_status posix_write_at(void *context, uint64_t offset,
                                  const void *buffer, size_t size)
{
    struct infs_posix_storage_context *posix = context;
    if (infs_pwrite_full(posix->fd, buffer, size, offset) != 0)
        return infs_status_from_errno(errno);
    return INFS_STATUS_OK;
}

static infs_status posix_flush(void *context)
{
    struct infs_posix_storage_context *posix = context;
    if (fsync(posix->fd) != 0)
        return infs_status_from_errno(errno);
    return INFS_STATUS_OK;
}

static infs_status posix_get_size(void *context, uint64_t *size_bytes,
                                  int *is_device)
{
    struct infs_posix_storage_context *posix = context;
    if (infs_get_size_bytes(posix->fd, size_bytes, is_device) != 0)
        return infs_status_from_errno(errno);
    return INFS_STATUS_OK;
}

static infs_status posix_random(void *context, void *buffer, size_t size)
{
    (void)context;
    if (infs_random_bytes(buffer, size) != 0)
        return infs_status_from_errno(errno);
    return INFS_STATUS_OK;
}

static infs_status posix_time(void *context, int64_t *time_ns)
{
    (void)context;
    errno = 0;
    *time_ns = infs_current_time_ns();
    if (*time_ns == 0 && errno != 0)
        return infs_status_from_errno(errno);
    return INFS_STATUS_OK;
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

infs_status infs_posix_storage_open(struct infs_storage *storage,
                                    const char *path, int writable)
{
    if (!storage || !path)
        return INFS_STATUS_INVALID_ARGUMENT;

    struct infs_posix_storage_context *context = malloc(sizeof(*context));
    if (!context)
        return INFS_STATUS_NO_MEMORY;
    context->fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (context->fd < 0) {
        infs_status status = infs_status_from_errno(errno);
        free(context);
        return status;
    }

    storage->ops = &posix_storage_ops;
    storage->context = context;
    return INFS_STATUS_OK;
}

infs_status infs_posix_volume_open(struct infs_volume *vol,
                                   const char *path, int writable)
{
    struct infs_storage storage = {0};
    infs_status status = infs_posix_storage_open(&storage, path, writable);
    if (status != INFS_STATUS_OK)
        return status;
    status = infs_volume_open_storage(vol, &storage, writable);
    if (status != INFS_STATUS_OK) {
        infs_storage_close(&storage);
        return status;
    }
    return INFS_STATUS_OK;
}
