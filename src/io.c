// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/io.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

int infs_pread_full(int fd, void *buf, size_t count, off_t offset)
{
    uint8_t *p = buf;
    while (count) {
        ssize_t n = pread(fd, p, count, offset);
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

int infs_pwrite_full(int fd, const void *buf, size_t count, off_t offset)
{
    const uint8_t *p = buf;
    while (count) {
        ssize_t n = pwrite(fd, p, count, offset);
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
