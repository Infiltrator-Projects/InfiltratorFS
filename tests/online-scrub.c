// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/endian.h"
#include "infilfs/format.h"
#include "infilfs/format_volume.h"
#include "infilfs/storage.h"
#include "infilfs/volume.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_BLOCKS UINT64_C(4096)
#define TEST_SIZE ((size_t)(TEST_BLOCKS * INFS_BLOCK_SIZE))

struct memory_image {
    uint8_t *bytes;
    size_t size;
    uint64_t random_state;
};

static void fail(const char *message)
{
    fprintf(stderr, "online-scrub: %s\n", message);
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
    struct memory_image *image = context;
    if (offset > image->size || size > image->size - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
    memcpy(buffer, image->bytes + (size_t)offset, size);
    return INFS_STATUS_OK;
}

static infs_status memory_write(void *context, uint64_t offset,
                                const void *buffer, size_t size)
{
    struct memory_image *image = context;
    if (offset > image->size || size > image->size - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
    memcpy(image->bytes + (size_t)offset, buffer, size);
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
    struct memory_image *image = context;
    *size_bytes = image->size;
    *is_device = 0;
    return INFS_STATUS_OK;
}

static infs_status memory_random(void *context, void *buffer, size_t size)
{
    struct memory_image *image = context;
    uint8_t *out = buffer;
    for (size_t i = 0; i < size; ++i) {
        image->random_state ^= image->random_state << 13;
        image->random_state ^= image->random_state >> 7;
        image->random_state ^= image->random_state << 17;
        out[i] = (uint8_t)image->random_state;
    }
    return INFS_STATUS_OK;
}

static infs_status memory_time(void *context, struct infs_timestamp *time)
{
    (void)context;
    time->seconds = INT64_C(1787288400);
    time->nanoseconds = UINT32_C(0);
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
    .current_time = memory_time,
    .close = memory_close,
};

static struct infs_storage make_storage(struct memory_image *image)
{
    struct infs_storage storage = { .ops = &memory_ops, .context = image };
    return storage;
}

int main(void)
{
    struct memory_image image = {
        .bytes = calloc(1, TEST_SIZE),
        .size = TEST_SIZE,
        .random_state = UINT64_C(0x6a09e667f3bcc909),
    };
    expect(image.bytes != NULL, "allocate image");
    struct infs_storage storage = make_storage(&image);
    expect(infs_format_storage(&storage, "online-scrub-test") == INFS_STATUS_OK,
           "format volume");

    struct infs_volume volume;
    storage = make_storage(&image);
    expect(infs_volume_open_storage(&volume, &storage, 1) == INFS_STATUS_OK,
           "open writable volume");

    uint8_t data[INFS_BLOCK_SIZE * 3u];
    for (size_t i = 0; i < sizeof(data); ++i)
        data[i] = (uint8_t)(i * 17u + 5u);
    expect(infs_create_file(&volume, "/stable", NULL) == INFS_STATUS_OK,
           "create stable file");
    expect(infs_write_file(&volume, "/stable", data, sizeof(data), 0) ==
               (int64_t)sizeof(data),
           "write stable file");

    uint64_t named_generation = infs_le64_to_cpu(volume.sb.generation);
    expect(infs_snapshot_create(&volume, "named-check") == INFS_STATUS_OK,
           "create named snapshot");
    struct infs_scrub_report named_report;
    expect(infs_snapshot_scrub(&volume, "named-check", &named_report) ==
               INFS_STATUS_OK,
           "scrub named snapshot");
    expect(named_report.scrub_generation == named_generation &&
           named_report.metadata_errors == 0 &&
           named_report.checksum_errors == 0,
           "named snapshot scrub is generation-stable and clean");

    struct infs_snapshot_info *before = NULL;
    size_t before_count = 0;
    expect(infs_snapshot_list(&volume, &before, &before_count) ==
               INFS_STATUS_OK,
           "list snapshots before online scrub");
    infs_free_snapshot_infos(before);

    uint64_t live_generation = infs_le64_to_cpu(volume.sb.generation);
    struct infs_scrub_report online_report;
    expect(infs_scrub_online(&volume, &online_report) == INFS_STATUS_OK,
           "snapshot-coordinated online scrub");
    expect(online_report.scrub_generation == live_generation &&
           online_report.metadata_errors == 0 &&
           online_report.checksum_errors == 0 &&
           online_report.files_checked != 0,
           "online scrub verifies captured committed generation");

    struct infs_snapshot_info *after = NULL;
    size_t after_count = 0;
    expect(infs_snapshot_list(&volume, &after, &after_count) == INFS_STATUS_OK,
           "list snapshots after online scrub");
    expect(after_count == before_count,
           "temporary online-scrub snapshot is removed");
    infs_free_snapshot_infos(after);

    expect(infs_snapshot_delete(&volume, "named-check") == INFS_STATUS_OK,
           "delete named snapshot");
    infs_volume_close(&volume);

    storage = make_storage(&image);
    expect(infs_volume_open_storage(&volume, &storage, 0) == INFS_STATUS_OK,
           "reopen read-only");
    expect(infs_scrub_online(&volume, &online_report) == INFS_STATUS_READ_ONLY,
           "online scrub requires writable retention coordination");
    infs_volume_close(&volume);
    free(image.bytes);
    puts("snapshot-coordinated online scrub: PASS");
    return 0;
}
