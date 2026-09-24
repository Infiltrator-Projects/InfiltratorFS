// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/storage_mirror.h"

#include <stdlib.h>
#include <string.h>

struct mirror_context {
    struct infs_storage *members;
    uint8_t *healthy;
    size_t count;
    uint64_t size_bytes;
    int is_device;
};

static infs_status mirror_read(void *opaque, uint64_t offset,
                               void *buffer, size_t size)
{
    struct mirror_context *ctx = opaque;
    infs_status first_error = INFS_STATUS_IO_ERROR;
    int have_copy = 0;
    uint8_t *verify = NULL;
    const size_t verify_capacity = size < 65536u ? size : 65536u;

    if (size) {
        verify = malloc(verify_capacity);
        if (!verify)
            return INFS_STATUS_NO_MEMORY;
    }

    for (size_t i = 0; i < ctx->count; ++i) {
        if (!ctx->healthy[i])
            continue;

        if (!have_copy) {
            infs_status status = infs_storage_read(
                &ctx->members[i], offset, buffer, size);
            if (status != INFS_STATUS_OK) {
                if (first_error == INFS_STATUS_IO_ERROR)
                    first_error = status;
                continue;
            }
            have_copy = 1;
            continue;
        }

        size_t checked = 0;
        while (checked < size) {
            size_t chunk = size - checked;
            if (chunk > verify_capacity)
                chunk = verify_capacity;
            infs_status status = infs_storage_read(
                &ctx->members[i], offset + checked, verify, chunk);
            if (status != INFS_STATUS_OK) {
                if (first_error == INFS_STATUS_IO_ERROR)
                    first_error = status;
                break;
            }
            if (memcmp((const uint8_t *)buffer + checked, verify, chunk) != 0) {
                free(verify);
                return INFS_STATUS_CORRUPT;
            }
            checked += chunk;
        }
    }

    free(verify);
    return have_copy ? INFS_STATUS_OK : first_error;
}

static infs_status mirror_write(void *opaque, uint64_t offset,
                                const void *buffer, size_t size)
{
    struct mirror_context *ctx = opaque;
    infs_status result = INFS_STATUS_OK;
    int attempted = 0;
    int degraded = 0;

    /*
     * Do not stop at the first failed member. Completing the write on every
     * still-working replica maximises the number of recoverable copies while
     * the returned failure tells the filesystem that durability is degraded.
     * Once a member has missed a write, keep subsequent writes fail-closed
     * until the mirror is explicitly reconstructed or repaired.
     */
    for (size_t i = 0; i < ctx->count; ++i) {
        if (!ctx->healthy[i]) {
            degraded = 1;
            continue;
        }
        attempted = 1;
        infs_status status = infs_storage_write(
            &ctx->members[i], offset, buffer, size);
        if (status != INFS_STATUS_OK) {
            ctx->healthy[i] = 0;
            degraded = 1;
            if (result == INFS_STATUS_OK)
                result = status;
        }
    }
    if (!attempted)
        return INFS_STATUS_IO_ERROR;
    return result != INFS_STATUS_OK ? result :
        (degraded ? INFS_STATUS_IO_ERROR : INFS_STATUS_OK);
}

static infs_status mirror_flush(void *opaque)
{
    struct mirror_context *ctx = opaque;
    infs_status result = INFS_STATUS_OK;
    int attempted = 0;
    int degraded = 0;

    for (size_t i = 0; i < ctx->count; ++i) {
        if (!ctx->healthy[i]) {
            degraded = 1;
            continue;
        }
        attempted = 1;
        infs_status status = infs_storage_flush(&ctx->members[i]);
        if (status != INFS_STATUS_OK) {
            ctx->healthy[i] = 0;
            degraded = 1;
            if (result == INFS_STATUS_OK)
                result = status;
        }
    }
    if (!attempted)
        return INFS_STATUS_IO_ERROR;
    return result != INFS_STATUS_OK ? result :
        (degraded ? INFS_STATUS_IO_ERROR : INFS_STATUS_OK);
}

static infs_status mirror_size(void *opaque, uint64_t *size_bytes,
                               int *is_device)
{
    struct mirror_context *ctx = opaque;
    *size_bytes = ctx->size_bytes;
    *is_device = ctx->is_device;
    return INFS_STATUS_OK;
}

static infs_status mirror_random(void *opaque, void *buffer, size_t size)
{
    struct mirror_context *ctx = opaque;
    for (size_t i = 0; i < ctx->count; ++i) {
        if (!ctx->members[i].ops->random_bytes)
            continue;
        infs_status status = infs_storage_random(
            &ctx->members[i], buffer, size);
        if (status == INFS_STATUS_OK)
            return status;
    }
    return INFS_STATUS_NOT_SUPPORTED;
}

static infs_status mirror_time(void *opaque, struct infs_timestamp *time)
{
    struct mirror_context *ctx = opaque;
    for (size_t i = 0; i < ctx->count; ++i) {
        if (!ctx->members[i].ops->current_time)
            continue;
        infs_status status = infs_storage_current_time(
            &ctx->members[i], time);
        if (status == INFS_STATUS_OK)
            return status;
    }
    return INFS_STATUS_NOT_SUPPORTED;
}

static void mirror_close(void *opaque)
{
    struct mirror_context *ctx = opaque;
    if (!ctx)
        return;
    for (size_t i = 0; i < ctx->count; ++i)
        infs_storage_close(&ctx->members[i]);
    free(ctx->healthy);
    free(ctx->members);
    free(ctx);
}

static const struct infs_storage_ops mirror_ops = {
    .read_at = mirror_read,
    .write_at = mirror_write,
    .flush = mirror_flush,
    .get_size = mirror_size,
    .random_bytes = mirror_random,
    .current_time = mirror_time,
    .close = mirror_close,
};

infs_status infs_storage_mirror_create(struct infs_storage *members,
                                       size_t member_count,
                                       struct infs_storage *out)
{
    if (!members || member_count < 2u || !out)
        return INFS_STATUS_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    struct mirror_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return INFS_STATUS_NO_MEMORY;
    ctx->members = calloc(member_count, sizeof(*ctx->members));
    ctx->healthy = calloc(member_count, sizeof(*ctx->healthy));
    if (!ctx->members || !ctx->healthy) {
        free(ctx->healthy);
        free(ctx->members);
        free(ctx);
        return INFS_STATUS_NO_MEMORY;
    }

    uint64_t minimum = UINT64_MAX;
    int all_devices = 1;
    for (size_t i = 0; i < member_count; ++i) {
        uint64_t bytes = 0;
        int device = 0;
        if (!infs_storage_valid(&members[i])) {
            free(ctx->healthy);
            free(ctx->members);
            free(ctx);
            return INFS_STATUS_INVALID_ARGUMENT;
        }
        infs_status status = infs_storage_get_size(
            &members[i], &bytes, &device);
        if (status != INFS_STATUS_OK || bytes == 0) {
            free(ctx->healthy);
            free(ctx->members);
            free(ctx);
            return status != INFS_STATUS_OK ?
                status : INFS_STATUS_INVALID_ARGUMENT;
        }
        if (bytes < minimum)
            minimum = bytes;
        if (!device)
            all_devices = 0;
    }

    memcpy(ctx->members, members, member_count * sizeof(*members));
    memset(ctx->healthy, 1, member_count * sizeof(*ctx->healthy));
    for (size_t i = 0; i < member_count; ++i) {
        members[i].ops = NULL;
        members[i].context = NULL;
    }
    ctx->count = member_count;
    ctx->size_bytes = minimum;
    ctx->is_device = all_devices;
    out->ops = &mirror_ops;
    out->context = ctx;
    return INFS_STATUS_OK;
}
