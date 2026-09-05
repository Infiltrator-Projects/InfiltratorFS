// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_POSIX_IO_H
#define INFILFS_POSIX_IO_H

#include <stddef.h>
#include <stdint.h>
#include "infilfs/time.h"

#include "infilfs/storage.h"

struct infs_volume;

infs_status infs_status_from_errno(int error_number);
int infs_status_to_errno(infs_status status);

int infs_pread_full(int fd, void *buf, size_t count, uint64_t offset);
int infs_pwrite_full(int fd, const void *buf, size_t count, uint64_t offset);
int infs_get_size_bytes(int fd, uint64_t *size_bytes, int *is_block_device);
int infs_random_bytes(void *buf, size_t count);
int infs_current_time(struct infs_timestamp *time);

infs_status infs_posix_storage_open(struct infs_storage *storage,
                                    const char *path, int writable);
infs_status infs_posix_volume_open(struct infs_volume *vol,
                                   const char *path, int writable);

#endif
