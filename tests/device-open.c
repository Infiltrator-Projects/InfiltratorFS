// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/format_volume.h"
#include "infilfs/storage.h"
#include "infilfs/volume.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_SIZE (UINT64_C(64) * 1024u * 1024u)
#define FILE_COUNT 220u

struct memory_device {
    uint8_t *bytes;
    size_t size;
    uint64_t random_state;
    uint64_t read_bytes;
    int is_device;
};

static void fail(const char *message)
{
    fprintf(stderr, "device-open: %s\n", message);
    exit(1);
}

static void expect(int condition, const char *message)
{
    if (!condition)
        fail(message);
}

static infs_status memory_read(void *context, uint64_t offset,
                               void *buffer, size_t size)
{
    struct memory_device *device = context;
    if (offset > device->size || size > device->size - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
    memcpy(buffer, device->bytes + (size_t)offset, size);
    device->read_bytes += size;
    return INFS_STATUS_OK;
}

static infs_status memory_write(void *context, uint64_t offset,
                                const void *buffer, size_t size)
{
    struct memory_device *device = context;
    if (offset > device->size || size > device->size - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
    memcpy(device->bytes + (size_t)offset, buffer, size);
    return INFS_STATUS_OK;
}

static infs_status memory_flush(void *context)
{
    (void)context;
    return INFS_STATUS_OK;
}

static infs_status memory_size(void *context, uint64_t *size_bytes,
                               int *is_device)
{
    struct memory_device *device = context;
    *size_bytes = device->size;
    *is_device = device->is_device;
    return INFS_STATUS_OK;
}

static infs_status memory_random(void *context, void *buffer, size_t size)
{
    struct memory_device *device = context;
    uint8_t *out = buffer;
    for (size_t i = 0; i < size; ++i) {
        device->random_state ^= device->random_state << 13;
        device->random_state ^= device->random_state >> 7;
        device->random_state ^= device->random_state << 17;
        out[i] = (uint8_t)device->random_state;
    }
    return INFS_STATUS_OK;
}

static infs_status memory_time(void *context, int64_t *time_ns)
{
    (void)context;
    *time_ns = INT64_C(1787350000000000000);
    return INFS_STATUS_OK;
}

static void memory_close(void *context)
{
    (void)context;
}

static const struct infs_storage_ops memory_ops = {
    .read_at = memory_read,
    .write_at = memory_write,
    .flush = memory_flush,
    .get_size = memory_size,
    .random_bytes = memory_random,
    .current_time_ns = memory_time,
    .close = memory_close,
};

static struct infs_storage make_storage(struct memory_device *device)
{
    struct infs_storage storage = {
        .ops = &memory_ops,
        .context = device,
    };
    return storage;
}

int main(void)
{
    struct memory_device device = {
        .bytes = calloc(1, (size_t)TEST_SIZE),
        .size = (size_t)TEST_SIZE,
        .random_state = UINT64_C(0x4d595df4d0f33173),
        .is_device = 0,
    };
    expect(device.bytes != NULL, "allocate image");

    struct infs_storage storage = make_storage(&device);
    expect(infs_format_storage(&storage, "device-open-test") == INFS_STATUS_OK,
           "format test volume");

    struct infs_volume volume;
    storage = make_storage(&device);
    expect(infs_volume_open_storage(&volume, &storage, 1) == INFS_STATUS_OK,
           "open image for population");
    for (unsigned i = 0; i < FILE_COUNT; ++i) {
        char path[96];
        snprintf(path, sizeof(path),
                 "/device-open-file-%03u-long-enough-to-page-directory.txt", i);
        expect(infs_create_file(&volume, path, NULL) == INFS_STATUS_OK,
               "populate scalable directory");
    }
    infs_volume_close(&volume);

    device.is_device = 1;
    device.read_bytes = 0;
    storage = make_storage(&device);
    expect(infs_volume_open_storage(&volume, &storage, 0) == INFS_STATUS_OK,
           "open populated block device");

    /* A device mount should read checkpoints, bitmap and only the root/index
     * metadata needed to become operational. It must not fsck-walk hundreds
     * of objects merely to open. */
    expect(device.read_bytes < UINT64_C(256) * 1024u,
           "device open performed a whole-volume metadata walk");

    struct infs_dir_item *items = NULL;
    size_t count = 0;
    expect(infs_list_dir(&volume, "/", &items, &count) == INFS_STATUS_OK,
           "list root after lightweight device open");
    expect(count == FILE_COUNT, "all files visible after lightweight open");
    infs_free_dir_items(items);

    struct infs_scrub_report report;
    expect(infs_scrub(&volume, &report) == INFS_STATUS_OK,
           "explicit scrub runs after lightweight open");
    expect(report.metadata_errors == 0 && report.checksum_errors == 0,
           "explicit scrub reports clean metadata");

    infs_volume_close(&volume);
    free(device.bytes);
    puts("real-device lightweight open: ok");
    return 0;
}
