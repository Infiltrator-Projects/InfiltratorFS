// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_STORAGE_H
#define INFILFS_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include "infilfs/status.h"
#include "infilfs/time.h"

/*
 * Portable storage contract.
 *
 * Callbacks are synchronous and operate on exact byte ranges. A successful
 * read_at/write_at must satisfy the complete requested size; short I/O is not
 * represented as success. Offsets and sizes are byte-based and the caller
 * guarantees that buffer remains valid for the duration of the callback.
 *
 * write_at, flush, random_bytes and current_time are required for writable
 * filesystem operation. Read-only consumers may omit mutation-only services.
 * flush is the durability boundary used by checkpoint publication: success
 * means writes issued before it have reached the backend's durable medium to
 * the extent that backend can guarantee. get_size returns the current backing
 * capacity and whether the backend is a block device.
 *
 * The storage object owns no implicit thread serialization. A backend used by
 * concurrent callers must either be inherently thread-safe or provide its own
 * synchronization. close releases backend context resources and is called at
 * most once by infs_storage_close().
 */
struct infs_storage_ops {
    infs_status (*read_at)(void *context, uint64_t offset,
                           void *buffer, size_t size);
    infs_status (*write_at)(void *context, uint64_t offset,
                            const void *buffer, size_t size);
    infs_status (*flush)(void *context);
    infs_status (*get_size)(void *context, uint64_t *size_bytes,
                            int *is_device);
    infs_status (*random_bytes)(void *context, void *buffer, size_t size);
    infs_status (*current_time)(void *context, struct infs_timestamp *time);
    void (*close)(void *context);
};

/*
 * infs_storage is a borrowed dispatch table plus an owned backend context.
 * Passing it into infs_volume_open_storage() transfers that context into the
 * volume on success; the caller's storage handle is cleared by that routine.
 */
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
infs_status infs_storage_current_time(const struct infs_storage *storage,
                                      struct infs_timestamp *time);
void infs_storage_close(struct infs_storage *storage);

#endif