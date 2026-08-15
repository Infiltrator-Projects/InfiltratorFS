// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/storage.h"

#include <errno.h>

int infs_storage_valid(const struct infs_storage *storage)
{
    return storage && storage->ops && storage->context &&
           storage->ops->read_at && storage->ops->get_size;
}

int infs_storage_read(const struct infs_storage *storage, uint64_t offset,
                      void *buffer, size_t size)
{
    if (!infs_storage_valid(storage) || (!buffer && size)) {
        errno = EINVAL;
        return -1;
    }
    return storage->ops->read_at(storage->context, offset, buffer, size);
}

int infs_storage_write(const struct infs_storage *storage, uint64_t offset,
                       const void *buffer, size_t size)
{
    if (!infs_storage_valid(storage) || !storage->ops->write_at ||
        (!buffer && size)) {
        errno = EINVAL;
        return -1;
    }
    return storage->ops->write_at(storage->context, offset, buffer, size);
}

int infs_storage_flush(const struct infs_storage *storage)
{
    if (!infs_storage_valid(storage) || !storage->ops->flush) {
        errno = EINVAL;
        return -1;
    }
    return storage->ops->flush(storage->context);
}

int infs_storage_get_size(const struct infs_storage *storage,
                          uint64_t *size_bytes, int *is_device)
{
    if (!infs_storage_valid(storage) || !size_bytes || !is_device) {
        errno = EINVAL;
        return -1;
    }
    return storage->ops->get_size(storage->context, size_bytes, is_device);
}

int infs_storage_random(const struct infs_storage *storage,
                        void *buffer, size_t size)
{
    if (!infs_storage_valid(storage) || !storage->ops->random_bytes ||
        (!buffer && size)) {
        errno = EINVAL;
        return -1;
    }
    return storage->ops->random_bytes(storage->context, buffer, size);
}

int64_t infs_storage_current_time_ns(const struct infs_storage *storage)
{
    if (!infs_storage_valid(storage) || !storage->ops->current_time_ns) {
        errno = EINVAL;
        return 0;
    }
    return storage->ops->current_time_ns(storage->context);
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
