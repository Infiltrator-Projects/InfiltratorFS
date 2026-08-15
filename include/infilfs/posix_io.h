// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_POSIX_IO_H
#define INFILFS_POSIX_IO_H

#include <stddef.h>
#include <stdint.h>

#include "infilfs/storage.h"

struct infs_volume;

int infs_pread_full(int fd, void *buf, size_t count, uint64_t offset);
int infs_pwrite_full(int fd, const void *buf, size_t count, uint64_t offset);
int infs_get_size_bytes(int fd, uint64_t *size_bytes, int *is_block_device);
int infs_random_bytes(void *buf, size_t count);
int64_t infs_current_time_ns(void);

int infs_posix_storage_open(struct infs_storage *storage,
                            const char *path, int writable);
int infs_posix_volume_open(struct infs_volume *vol,
                           const char *path, int writable);

#endif
