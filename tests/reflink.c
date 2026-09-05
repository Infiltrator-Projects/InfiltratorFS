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

#define TEST_BLOCKS UINT64_C(4096)
#define TEST_SIZE ((size_t)(TEST_BLOCKS * INFS_BLOCK_SIZE))

struct memory_image {
    uint8_t *bytes;
    size_t size;
    uint64_t random_state;
};

static void fail(const char *message)
{
    fprintf(stderr, "reflink: %s\n", message);
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
    struct infs_storage storage = {
        .ops = &memory_ops,
        .context = image,
    };
    return storage;
}

static int bitmap_get_test(const uint8_t *bitmap, uint64_t block)
{
    return (bitmap[block >> 3] >> (block & 7u)) & 1u;
}

static uint64_t physical_block_for(struct infs_volume *vol,
                                   struct memory_image *image,
                                   const char *path, uint64_t logical,
                                   int *compressed_out)
{
    struct infs_lookup lookup;
    expect(infs_lookup_path(vol, path, &lookup) == INFS_STATUS_OK,
           "lookup file for physical extent");
    uint8_t *object = image->bytes + lookup.block * INFS_BLOCK_SIZE;
    expect(infs_validate_object_block(object), "validate file object");
    struct infs_file_payload_disk *file =
        (struct infs_file_payload_disk *)(
            object + sizeof(struct infs_object_header_disk));
    struct infs_extent_disk *ext = (struct infs_extent_disk *)(file + 1);
    uint32_t count = infs_le32_to_cpu(file->extent_count);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t first = infs_le64_to_cpu(ext[i].logical_block);
        uint32_t blocks = infs_le32_to_cpu(ext[i].block_count);
        if (logical < first || logical - first >= blocks)
            continue;
        uint32_t flags = infs_le32_to_cpu(ext[i].flags);
        expect((flags & INFS_EXTENT_KIND_MASK) == INFS_EXTENT_NORMAL,
               "expected data extent");
        int compressed =
            ((flags & INFS_EXTENT_CODEC_MASK) >> INFS_EXTENT_CODEC_SHIFT) !=
            INFS_COMPRESSION_NONE;
        if (compressed_out)
            *compressed_out = compressed;
        return infs_le64_to_cpu(ext[i].physical_block) +
            (compressed ? 0u : logical - first);
    }
    fail("logical block not mapped");
    return 0;
}

int main(void)
{
    struct memory_image image = {
        .bytes = calloc(1, TEST_SIZE),
        .size = TEST_SIZE,
        .random_state = UINT64_C(0x9e3779b97f4a7c15),
    };
    expect(image.bytes != NULL, "allocate image");

    struct infs_storage storage = make_storage(&image);
    expect(infs_format_storage(&storage, "reflink-test") == INFS_STATUS_OK,
           "format shared-extent volume");

    struct infs_volume vol;
    storage = make_storage(&image);
    expect(infs_volume_open_storage(&vol, &storage, 1) == INFS_STATUS_OK,
           "open writable volume");
    expect((infs_le64_to_cpu(vol.sb.incompat_flags) &
            INFS_INCOMPAT_SHARED_EXTENTS) != 0,
           "shared-extent feature enabled");

    uint8_t source_data[INFS_BLOCK_SIZE * 2u];
    uint8_t changed[INFS_BLOCK_SIZE];
    for (size_t i = 0; i < sizeof(source_data); ++i)
        source_data[i] = (uint8_t)(i * 17u + 3u);
    memset(changed, 0xa5, sizeof(changed));

    expect(infs_create_file(&vol, "/source", NULL) == INFS_STATUS_OK,
           "create source");
    expect(infs_write_file(&vol, "/source", source_data,
                           sizeof(source_data), 0) ==
               (int64_t)sizeof(source_data),
           "write source");

    int source_compressed = 0;
    uint64_t source0 = physical_block_for(
        &vol, &image, "/source", 0, &source_compressed);
    uint64_t source1 = physical_block_for(&vol, &image, "/source", 1, NULL);
    expect(infs_reflink_file(&vol, "/source", "/clone") == INFS_STATUS_OK,
           "create reflink clone");
    expect(physical_block_for(&vol, &image, "/clone", 0, NULL) == source0,
           "clone shares first data block");
    expect(physical_block_for(&vol, &image, "/clone", 1, NULL) == source1,
           "clone shares second data block");

    uint8_t readback[sizeof(source_data)];
    expect(infs_read_file(&vol, "/clone", readback, sizeof(readback), 0) ==
               (int64_t)sizeof(readback),
           "read clone");
    expect(memcmp(readback, source_data, sizeof(readback)) == 0,
           "clone data matches source");

    {
        int64_t rc = infs_write_file(
            &vol, "/clone", changed, sizeof(changed), 0);
        if (rc != (int64_t)sizeof(changed)) {
            fprintf(stderr,
                    "reflink: clone write returned %lld (expected %zu)\n",
                    (long long)rc, sizeof(changed));
            fail("break sharing on clone write");
        }
    }
    uint64_t clone0 = physical_block_for(&vol, &image, "/clone", 0, NULL);
    expect(clone0 != source0, "changed clone block is private");
    if (source_compressed) {
        expect(physical_block_for(&vol, &image, "/clone", 1, NULL) != source1,
               "partial clone write materializes bounded compressed cluster");
    } else {
        expect(physical_block_for(&vol, &image, "/clone", 1, NULL) == source1,
               "unchanged uncompressed clone block remains shared");
    }
    expect(infs_read_file(&vol, "/source", readback, sizeof(readback), 0) ==
               (int64_t)sizeof(readback),
           "read source after clone write");
    expect(memcmp(readback, source_data, sizeof(readback)) == 0,
           "source unchanged after clone write");

    expect(infs_truncate_file(&vol, "/clone", INFS_BLOCK_SIZE) ==
               INFS_STATUS_OK,
           "truncate clone");
    expect(bitmap_get_test(vol.bitmap, source1),
           "shared source block retained after clone truncate");

    expect(infs_unlink(&vol, "/source") == INFS_STATUS_OK,
           "unlink source");
    expect(!bitmap_get_test(vol.bitmap, source0),
           "unshared old source block reclaimed");
    expect(!bitmap_get_test(vol.bitmap, source1),
           "last reference reclaimed after source unlink");
    expect(bitmap_get_test(vol.bitmap, clone0),
           "clone private block retained");

    uint8_t clone_read[INFS_BLOCK_SIZE];
    expect(infs_read_file(&vol, "/clone", clone_read, sizeof(clone_read), 0) ==
               (int64_t)sizeof(clone_read),
           "read surviving clone");
    expect(memcmp(clone_read, changed, sizeof(changed)) == 0,
           "surviving clone data correct");

    expect(infs_create_file(&vol, "/tiny", NULL) == INFS_STATUS_OK,
           "create tiny source");
    expect(infs_write_file(&vol, "/tiny", "mesh", 4, 0) == 4,
           "write tiny source");
    expect(infs_reflink_file(&vol, "/tiny", "/tiny-clone") == INFS_STATUS_OK,
           "clone inline source");
    char tiny[4];
    expect(infs_read_file(&vol, "/tiny-clone", tiny, sizeof(tiny), 0) == 4,
           "read inline clone");
    expect(memcmp(tiny, "mesh", 4) == 0, "inline clone data correct");

    struct infs_scrub_report report;
    expect(infs_scrub(&vol, &report) == INFS_STATUS_OK, "scrub reflink volume");
    expect(report.checksum_errors == 0 && report.metadata_errors == 0,
           "scrub reports clean reflink volume");

    infs_volume_close(&vol);
    storage = make_storage(&image);
    expect(infs_volume_open_storage(&vol, &storage, 0) == INFS_STATUS_OK,
           "reopen reflink volume read-only");
    expect(infs_read_file(&vol, "/clone", clone_read, sizeof(clone_read), 0) ==
               (int64_t)sizeof(clone_read),
           "read clone after remount");
    expect(memcmp(clone_read, changed, sizeof(changed)) == 0,
           "clone persists across remount");
    infs_volume_close(&vol);

    free(image.bytes);
    puts("reflink conformance: ok");
    return 0;
}
