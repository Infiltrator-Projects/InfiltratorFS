// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/endian.h"
#include "infilfs/format.h"
#include "infilfs/format_volume.h"
#include "infilfs/fs.h"
#include "infilfs/storage.h"
#include "infilfs/volume.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_BYTES (UINT64_C(16) * 1024u * 1024u)
#define CLASSIC_EXTENT_COUNT 160u
#define LOGICAL_BLOCKS 160u

struct memory_image {
    uint8_t *bytes;
    size_t size;
    uint64_t random_state;
};

static void fail(const char *message)
{
    fprintf(stderr, "eof-extent-promotion: %s\n", message);
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
    time->seconds = INT64_C(1787634000);
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
    struct infs_storage storage = {
        .ops = &memory_ops,
        .context = image,
    };
    return storage;
}

int main(void)
{
    struct memory_image image = {
        .bytes = calloc(1, (size_t)TEST_BYTES),
        .size = (size_t)TEST_BYTES,
        .random_state = UINT64_C(0x243f6a8885a308d3),
    };
    expect(image.bytes != NULL, "allocate memory image");

    struct infs_storage storage = make_storage(&image);
    expect(infs_format_storage(&storage, "eof-promotion") == INFS_STATUS_OK,
           "format image");

    struct infs_volume volume;
    storage = make_storage(&image);
    expect(infs_volume_open_storage(&volume, &storage, 1) == INFS_STATUS_OK,
           "open writable volume");
    expect(infs_create_file(&volume, "/growth.bin", NULL) == INFS_STATUS_OK,
           "create growth file");
    expect(infs_truncate_file(&volume, "/growth.bin",
                              (uint64_t)LOGICAL_BLOCKS * INFS_BLOCK_SIZE) ==
               INFS_STATUS_OK,
           "create initial sparse range");
    expect(infs_volume_set_deferred_publish(
               &volume, 1, UINT64_C(8) * 1024u * 1024u) == INFS_STATUS_OK,
           "enable deferred publication");

    uint8_t block[INFS_BLOCK_SIZE];
    memset(block, 0x5a, sizeof(block));

    /* Write every odd logical block.  After the final block (159) this is
     * exactly 160 alternating HOLE/NORMAL extents and the tail is NORMAL.
     * It deliberately remains a classic file right at the Format 0.18 inline
     * extent capacity so that the next EOF append must perform classic->paged
     * promotion inside file_ensure_logical_blocks(). */
    for (uint64_t logical = 1; logical < LOGICAL_BLOCKS; logical += 2u) {
        block[0] = (uint8_t)logical;
        expect(infs_write_file_buffered(
                   &volume, "/growth.bin", block, sizeof(block),
                   logical * INFS_BLOCK_SIZE) == (int64_t)sizeof(block),
               "build exact classic extent ceiling");
    }

    struct infs_lookup lookup;
    expect(infs_lookup_path(&volume, "/growth.bin", &lookup) == INFS_STATUS_OK,
           "lookup classic growth file");
    const uint8_t *object = image.bytes + lookup.block * INFS_BLOCK_SIZE;
    const struct infs_object_header_disk *header =
        (const struct infs_object_header_disk *)object;
    const struct infs_file_payload_disk *file =
        (const struct infs_file_payload_disk *)(header + 1);
    expect(infs_le16_to_cpu(header->object_version) ==
               INFS_OBJECT_VERSION_CLASSIC,
           "file remains classic at exact inline extent ceiling");
    expect(infs_le32_to_cpu(file->extent_count) == CLASSIC_EXTENT_COUNT,
           "classic file has exactly 160 extents");

    memset(block, 0xa5, sizeof(block));
    expect(infs_write_file_buffered(
               &volume, "/growth.bin", block, sizeof(block),
               (uint64_t)LOGICAL_BLOCKS * INFS_BLOCK_SIZE) ==
               (int64_t)sizeof(block),
           "append beyond classic extent ceiling");
    expect(infs_volume_sync(&volume) == INFS_STATUS_OK,
           "publish promoted EOF append");

    expect(infs_lookup_path(&volume, "/growth.bin", &lookup) == INFS_STATUS_OK,
           "lookup promoted growth file");
    object = image.bytes + lookup.block * INFS_BLOCK_SIZE;
    header = (const struct infs_object_header_disk *)object;
    file = (const struct infs_file_payload_disk *)(header + 1);
    expect(infs_le16_to_cpu(header->object_version) == INFS_OBJECT_VERSION_PAGED,
           "EOF growth promoted file to paged extents");

    uint8_t readback[INFS_BLOCK_SIZE];
    expect(infs_read_file(&volume, "/growth.bin", readback, sizeof(readback),
                          (uint64_t)LOGICAL_BLOCKS * INFS_BLOCK_SIZE) ==
               (int64_t)sizeof(readback),
           "read appended block after promotion");
    expect(memcmp(readback, block, sizeof(block)) == 0,
           "promoted EOF append data matches");

    struct infs_scrub_report report;
    expect(infs_scrub(&volume, &report) == INFS_STATUS_OK &&
               report.checksum_errors == 0 && report.metadata_errors == 0,
           "scrub promoted EOF-growth file");

    infs_volume_close(&volume);
    free(image.bytes);
    puts("eof-extent-promotion: PASS");
    return 0;
}
