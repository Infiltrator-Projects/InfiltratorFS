// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/storage.h"

int infs_storage_valid(const struct infs_storage *storage)
{
    return storage && storage->ops && storage->context &&
           storage->ops->read_at && storage->ops->get_size;
}

infs_status infs_storage_read(const struct infs_storage *storage,
                              uint64_t offset, void *buffer, size_t size)
{
    if (!infs_storage_valid(storage) || (!buffer && size)) {
        return INFS_STATUS_INVALID_ARGUMENT;
    }
    return storage->ops->read_at(storage->context, offset, buffer, size);
}

infs_status infs_storage_write(const struct infs_storage *storage,
                               uint64_t offset, const void *buffer, size_t size)
{
    if (!infs_storage_valid(storage) || !storage->ops->write_at ||
        (!buffer && size)) {
        return INFS_STATUS_INVALID_ARGUMENT;
    }
    return storage->ops->write_at(storage->context, offset, buffer, size);
}

infs_status infs_storage_flush(const struct infs_storage *storage)
{
    if (!infs_storage_valid(storage) || !storage->ops->flush) {
        return INFS_STATUS_INVALID_ARGUMENT;
    }
    return storage->ops->flush(storage->context);
}

infs_status infs_storage_get_size(const struct infs_storage *storage,
                                  uint64_t *size_bytes, int *is_device)
{
    if (!infs_storage_valid(storage) || !size_bytes || !is_device) {
        return INFS_STATUS_INVALID_ARGUMENT;
    }
    return storage->ops->get_size(storage->context, size_bytes, is_device);
}

infs_status infs_storage_random(const struct infs_storage *storage,
                                void *buffer, size_t size)
{
    if (!infs_storage_valid(storage) || !storage->ops->random_bytes ||
        (!buffer && size)) {
        return INFS_STATUS_INVALID_ARGUMENT;
    }
    return storage->ops->random_bytes(storage->context, buffer, size);
}

infs_status infs_storage_current_time_ns(const struct infs_storage *storage,
                                         int64_t *time_ns)
{
    if (!infs_storage_valid(storage) || !storage->ops->current_time_ns ||
        !time_ns)
        return INFS_STATUS_INVALID_ARGUMENT;
    return storage->ops->current_time_ns(storage->context, time_ns);
}

void infs_storage_close(struct infs_storage *storage)
{
    if (!storage)
        return;
    if (storage->ops && storage->ops->close && storage->context)
        storage->ops->close(storage->context);
    storage->ops = NULL;
    storage->context = NULL;
}
