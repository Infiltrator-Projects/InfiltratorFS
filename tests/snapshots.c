// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/endian.h"
#include "infilfs/format.h"
#include "infilfs/format_volume.h"
#include "infilfs/fs.h"
#include "infilfs/storage.h"
#include "infilfs/volume.h"

#include <stdint.h>
#include <inttypes.h>
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
    fprintf(stderr, "snapshots: %s\n", message);
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

static infs_status memory_time(void *context, int64_t *time_ns)
{
    (void)context;
    *time_ns = INT64_C(1787288400000000000);
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

static struct infs_storage make_storage(struct memory_image *image)
{
    struct infs_storage storage = {
        .ops = &memory_ops,
        .context = image,
    };
    return storage;
}

static uint64_t snapshot_data_block(struct infs_volume *volume,
                                    struct memory_image *image,
                                    const char *snapshot, const char *path)
{
    struct infs_lookup lookup;
    expect(infs_snapshot_lookup_path(volume, snapshot, path, &lookup) ==
               INFS_STATUS_OK,
           "lookup historical file for corruption test");
    uint8_t *object = image->bytes + lookup.block * INFS_BLOCK_SIZE;
    expect(infs_validate_object_block(object), "historical file object valid");
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)object;
    struct infs_file_payload_disk *file =
        (struct infs_file_payload_disk *)(header + 1);
    expect(infs_le32_to_cpu(file->extent_count) != 0,
           "historical file uses extents");
    struct infs_extent_disk *extent = (struct infs_extent_disk *)(file + 1);
    expect(infs_le32_to_cpu(extent->flags) == INFS_EXTENT_NORMAL,
           "historical first extent is normal");
    return infs_le64_to_cpu(extent->physical_block);
}

static uint64_t current_snapshot_catalog_block(struct infs_volume *volume,
                                               struct memory_image *image)
{
    for (uint64_t block = 1; block + 1u < TEST_BLOCKS; ++block) {
        if (((volume->bitmap[block >> 3] >> (block & 7u)) & 1u) == 0)
            continue;
        uint8_t *raw = image->bytes + block * INFS_BLOCK_SIZE;
        if (!infs_validate_object_block(raw))
            continue;
        struct infs_object_header_disk *header =
            (struct infs_object_header_disk *)raw;
        if (infs_le16_to_cpu(header->object_type) ==
            INFS_OBJECT_SNAPSHOT_CATALOG)
            return block;
    }
    fail("locate current snapshot catalog");
    return 0;
}

static void check_format_010_compatibility(void)
{
    struct memory_image image = {
        .bytes = calloc(1, TEST_SIZE),
        .size = TEST_SIZE,
        .random_state = UINT64_C(0x243f6a8885a308d3),
    };
    expect(image.bytes != NULL, "allocate Format 0.10 image");
    struct infs_storage storage = make_storage(&image);
    expect(infs_format_storage(&storage, "format-0.10") == INFS_STATUS_OK,
           "format compatibility image");

    const uint64_t checkpoints[INFS_CHECKPOINT_COUNT] = {
        0, TEST_BLOCKS / 2u, TEST_BLOCKS - 1u
    };
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        uint8_t *raw = image.bytes + checkpoints[i] * INFS_BLOCK_SIZE;
        struct infs_superblock_disk sb;
        expect(infs_decode_superblock(raw, &sb) == INFS_STATUS_OK,
               "decode compatibility checkpoint");
        sb.format_minor = infs_cpu_to_le16(10u);
        sb.incompat_flags = infs_cpu_to_le64(
            infs_le64_to_cpu(sb.incompat_flags) & ~INFS_INCOMPAT_SNAPSHOTS);
        expect(infs_encode_superblock(raw, &sb) == INFS_STATUS_OK,
               "encode Format 0.10 checkpoint");
    }

    struct infs_volume volume;
    storage = make_storage(&image);
    expect(infs_volume_open_storage(&volume, &storage, 1) == INFS_STATUS_OK,
           "Format 0.10 remains writable");
    expect(infs_snapshot_create(&volume, "unsupported") ==
               INFS_STATUS_NOT_SUPPORTED,
           "Format 0.10 rejects snapshot creation");
    infs_volume_close(&volume);
    free(image.bytes);
}

int main(void)
{
    check_format_010_compatibility();
    struct memory_image image = {
        .bytes = calloc(1, TEST_SIZE),
        .size = TEST_SIZE,
        .random_state = UINT64_C(0x9e3779b97f4a7c15),
    };
    expect(image.bytes != NULL, "allocate image");
    struct infs_storage storage = make_storage(&image);
    expect(infs_format_storage(&storage, "snapshot-test") == INFS_STATUS_OK,
           "format volume");

    struct infs_volume volume;
    storage = make_storage(&image);
    expect(infs_volume_open_storage(&volume, &storage, 1) == INFS_STATUS_OK,
           "open writable volume");
    expect(infs_le16_to_cpu(volume.sb.format_minor) == 11u,
           "Format minor is 0.11");
    expect((infs_le64_to_cpu(volume.sb.incompat_flags) &
            INFS_INCOMPAT_SNAPSHOTS) != 0,
           "snapshot feature enabled");

    uint8_t original[INFS_BLOCK_SIZE + 137u];
    uint8_t changed[sizeof(original)];
    for (size_t i = 0; i < sizeof(original); ++i) {
        original[i] = (uint8_t)(i * 29u + 7u);
        changed[i] = (uint8_t)(i * 11u + 3u);
    }
    expect(infs_create_file(&volume, "/history", NULL) == INFS_STATUS_OK,
           "create historical file");
    expect(infs_write_file(&volume, "/history", original, sizeof(original), 0) ==
               (int64_t)sizeof(original),
           "write historical file");
    expect(infs_create_symlink(&volume, "/pointer", "history", NULL) ==
               INFS_STATUS_OK,
           "create historical symbolic link");

    uint64_t captured_generation = infs_le64_to_cpu(volume.sb.generation);
    expect(infs_snapshot_create(&volume, "before-edit") == INFS_STATUS_OK,
           "create first snapshot");
    expect(infs_snapshot_create(&volume, "before-edit") ==
               INFS_STATUS_ALREADY_EXISTS,
           "reject duplicate snapshot name");
    expect(infs_snapshot_create(&volume, "bad/name") ==
               INFS_STATUS_INVALID_ARGUMENT,
           "reject invalid snapshot name");

    struct infs_snapshot_info *snapshots = NULL;
    size_t snapshot_count = 0;
    expect(infs_snapshot_list(&volume, &snapshots, &snapshot_count) ==
               INFS_STATUS_OK,
           "list first snapshot");
    expect(snapshot_count == 1 &&
           strcmp(snapshots[0].name, "before-edit") == 0 &&
           snapshots[0].generation == captured_generation,
           "snapshot records captured generation");
    infs_free_snapshot_infos(snapshots);

    struct infs_scrub_report prewrite_report;
    infs_status prewrite_status = infs_scrub(&volume, &prewrite_report);
    if (prewrite_status != INFS_STATUS_OK || prewrite_report.metadata_errors ||
        prewrite_report.checksum_errors) {
        fprintf(stderr, "post-create scrub: %s metadata=%" PRIu64
                        " checksum=%" PRIu64 "\n",
                infs_status_string(prewrite_status),
                prewrite_report.metadata_errors,
                prewrite_report.checksum_errors);
        fail("snapshot graph valid immediately after creation");
    }

    int64_t changed_result = infs_write_file(
        &volume, "/history", changed, sizeof(changed), 0);
    if (changed_result != (int64_t)sizeof(changed)) {
        fprintf(stderr, "snapshot write result: %" PRId64 " (%s)\n",
                changed_result,
                changed_result < 0 ? infs_status_string((infs_status)changed_result) :
                                     "short write");
        fail("replace live data after snapshot");
    }
    expect(infs_rename(&volume, "/history", "/current") == INFS_STATUS_OK,
           "rename live file after snapshot");
    expect(infs_unlink(&volume, "/pointer") == INFS_STATUS_OK,
           "unlink live symbolic link after snapshot");

    uint8_t readback[sizeof(original)];
    expect(infs_snapshot_read_file(&volume, "before-edit", "/history",
                                   readback, sizeof(readback), 0) ==
               (int64_t)sizeof(readback) &&
           memcmp(readback, original, sizeof(readback)) == 0,
           "first snapshot retains original data and name");
    char target[32];
    size_t target_length = 0;
    expect(infs_snapshot_read_symlink(&volume, "before-edit", "/pointer",
                                      target, sizeof(target), &target_length) ==
               INFS_STATUS_OK && strcmp(target, "history") == 0,
           "first snapshot retains symbolic link");
    struct infs_lookup lookup;
    expect(infs_snapshot_lookup_path(&volume, "before-edit", "/current",
                                     &lookup) == INFS_STATUS_NOT_FOUND,
           "first snapshot excludes later rename");

    expect(infs_snapshot_create(&volume, "after-edit") == INFS_STATUS_OK,
           "create nested retained generation");
    expect(infs_unlink(&volume, "/current") == INFS_STATUS_OK,
           "remove live file after second snapshot");
    expect(infs_snapshot_read_file(&volume, "after-edit", "/current",
                                   readback, sizeof(readback), 0) ==
               (int64_t)sizeof(readback) &&
           memcmp(readback, changed, sizeof(readback)) == 0,
           "second snapshot retains changed data");

    struct infs_scrub_report report;
    expect(infs_scrub(&volume, &report) == INFS_STATUS_OK &&
           report.metadata_errors == 0 && report.checksum_errors == 0 &&
           report.files_checked >= 2,
           "scrub validates live and retained generations");

    infs_volume_close(&volume);
    storage = make_storage(&image);
    expect(infs_volume_open_storage(&volume, &storage, 1) == INFS_STATUS_OK,
           "remount snapshot volume");
    expect(infs_snapshot_read_file(&volume, "before-edit", "/history",
                                   readback, sizeof(readback), 0) ==
               (int64_t)sizeof(readback) &&
           memcmp(readback, original, sizeof(readback)) == 0,
           "first snapshot survives remount");

    uint8_t *metadata_copy = malloc(TEST_SIZE);
    expect(metadata_copy != NULL, "allocate metadata-corruption image");
    memcpy(metadata_copy, image.bytes, TEST_SIZE);
    struct memory_image metadata_corrupt = {
        .bytes = metadata_copy,
        .size = TEST_SIZE,
        .random_state = UINT64_C(0x0f1e2d3c4b5a6978),
    };
    uint64_t catalog_block = current_snapshot_catalog_block(
        &volume, &metadata_corrupt);
    uint8_t *catalog_raw = metadata_corrupt.bytes +
        catalog_block * INFS_BLOCK_SIZE;
    struct infs_object_header_disk *catalog_header =
        (struct infs_object_header_disk *)catalog_raw;
    struct infs_snapshot_catalog_payload_disk *catalog_payload =
        (struct infs_snapshot_catalog_payload_disk *)(catalog_header + 1);
    struct infs_snapshot_record_disk *catalog_records =
        (struct infs_snapshot_record_disk *)(catalog_payload + 1);
    catalog_records[0].generation = volume.sb.generation;
    expect(infs_object_finalize(catalog_raw) == INFS_STATUS_OK,
           "rechecksum malformed snapshot catalog");
    struct infs_volume metadata_corrupt_volume;
    storage = make_storage(&metadata_corrupt);
    expect(infs_volume_open_storage(&metadata_corrupt_volume, &storage, 0) ==
               INFS_STATUS_CORRUPT,
           "reject checksummed invalid snapshot generation ordering");
    free(metadata_copy);

    uint8_t *corrupt_copy = malloc(TEST_SIZE);
    expect(corrupt_copy != NULL, "allocate corruption image");
    memcpy(corrupt_copy, image.bytes, TEST_SIZE);
    struct memory_image corrupt = {
        .bytes = corrupt_copy,
        .size = TEST_SIZE,
        .random_state = UINT64_C(0x123456789abcdef0),
    };
    uint64_t historical_data = snapshot_data_block(
        &volume, &image, "before-edit", "/history");
    corrupt.bytes[historical_data * INFS_BLOCK_SIZE + 17u] ^= UINT8_C(0x5a);
    struct infs_volume corrupt_volume;
    storage = make_storage(&corrupt);
    expect(infs_volume_open_storage(&corrupt_volume, &storage, 0) ==
               INFS_STATUS_OK,
           "metadata-valid snapshot opens before data scrub");
    expect(infs_scrub(&corrupt_volume, &report) == INFS_STATUS_OK &&
           report.checksum_errors != 0,
           "scrub detects retained-generation data corruption");
    infs_volume_close(&corrupt_volume);
    free(corrupt_copy);

    uint64_t free_before_delete = infs_le64_to_cpu(volume.sb.free_blocks);
    expect(infs_snapshot_delete(&volume, "before-edit") == INFS_STATUS_OK,
           "delete first snapshot");
    expect(infs_snapshot_lookup_path(&volume, "before-edit", "/history",
                                     &lookup) == INFS_STATUS_NOT_FOUND,
           "deleted snapshot is unavailable");
    expect(infs_snapshot_read_file(&volume, "after-edit", "/current",
                                   readback, sizeof(readback), 0) ==
               (int64_t)sizeof(readback),
           "newer snapshot retains older graph dependencies");
    expect(infs_snapshot_delete(&volume, "after-edit") == INFS_STATUS_OK,
           "delete final snapshot");
    expect(infs_le64_to_cpu(volume.sb.free_blocks) > free_before_delete,
           "final deletion reclaims retained blocks");
    snapshots = NULL;
    snapshot_count = 1;
    expect(infs_snapshot_list(&volume, &snapshots, &snapshot_count) ==
               INFS_STATUS_OK && snapshot_count == 0,
           "snapshot catalog is empty");
    infs_free_snapshot_infos(snapshots);
    expect(infs_scrub(&volume, &report) == INFS_STATUS_OK &&
           report.metadata_errors == 0 && report.checksum_errors == 0,
           "scrub clean after reclamation");

    infs_volume_close(&volume);
    free(image.bytes);
    puts("snapshot roots and retained generations: PASS");
    return 0;
}
