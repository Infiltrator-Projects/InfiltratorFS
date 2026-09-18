// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_POSIX_IO_H
#define INFILFS_POSIX_IO_H

#include <stddef.h>
#include <stdint.h>
#include "infilfs/time.h"
#include "infiltratr/posix_io.h"

#include "infilfs/storage.h"

struct infs_volume;

/* POSIX adapter contract:
 * - exact-I/O aliases return 0 on complete transfer and -1 with errno set;
 * - status conversion is intentionally lossy where POSIX lacks a distinct
 *   errno for a portable filesystem status;
 * - storage_open() acquires a non-blocking shared/read-only or exclusive/
 *   writable advisory lock and owns the descriptor until storage_close();
 * - volume_open() transfers the opened storage context into the volume on
 *   success; infs_volume_close() then releases it. */

infs_status infs_status_from_errno(int error_number);
int infs_status_to_errno(infs_status status);

#define infs_pread_full infiltratr_pread_full
#define infs_pwrite_full infiltratr_pwrite_full
int infs_get_size_bytes(int fd, uint64_t *size_bytes, int *is_block_device);
int infs_random_bytes(void *buf, size_t count);
int infs_current_time(struct infs_timestamp *time);

infs_status infs_posix_storage_open(struct infs_storage *storage,
                                    const char *path, int writable);
infs_status infs_posix_volume_open(struct infs_volume *vol,
                                   const char *path, int writable);

#endif
