// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_IO_H
#define INFILFS_IO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

int infs_pread_full(int fd, void *buf, size_t count, off_t offset);
int infs_pwrite_full(int fd, const void *buf, size_t count, off_t offset);
int infs_get_size_bytes(int fd, uint64_t *size_bytes, int *is_block_device);
int infs_random_bytes(void *buf, size_t count);

#endif
