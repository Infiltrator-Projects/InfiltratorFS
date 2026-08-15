// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_STORAGE_H
#define INFILFS_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include "infilfs/status.h"

struct infs_storage_ops {
    infs_status (*read_at)(void *context, uint64_t offset,
                           void *buffer, size_t size);
    infs_status (*write_at)(void *context, uint64_t offset,
                            const void *buffer, size_t size);
    infs_status (*flush)(void *context);
    infs_status (*get_size)(void *context, uint64_t *size_bytes,
                            int *is_device);
    infs_status (*random_bytes)(void *context, void *buffer, size_t size);
    infs_status (*current_time_ns)(void *context, int64_t *time_ns);
    void (*close)(void *context);
};

struct infs_storage {
    const struct infs_storage_ops *ops;
    void *context;
};

int infs_storage_valid(const struct infs_storage *storage);
infs_status infs_storage_read(const struct infs_storage *storage,
                              uint64_t offset, void *buffer, size_t size);
infs_status infs_storage_write(const struct infs_storage *storage,
                               uint64_t offset, const void *buffer, size_t size);
infs_status infs_storage_flush(const struct infs_storage *storage);
infs_status infs_storage_get_size(const struct infs_storage *storage,
                                  uint64_t *size_bytes, int *is_device);
infs_status infs_storage_random(const struct infs_storage *storage,
                                void *buffer, size_t size);
infs_status infs_storage_current_time_ns(const struct infs_storage *storage,
                                         int64_t *time_ns);
void infs_storage_close(struct infs_storage *storage);

#endif
