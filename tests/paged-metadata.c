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

#define TEST_BLOCKS UINT64_C(8192)
#define TEST_SIZE ((size_t)(TEST_BLOCKS * INFS_BLOCK_SIZE))
#define FILE_COUNT 220u

struct memory_image {
    uint8_t *bytes;
    size_t size;
    uint64_t random_state;
    uint64_t flushes;
};

static void fail(const char *message)
{
    fprintf(stderr, "paged-metadata: %s\n", message);
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
    struct memory_image *image = context;
    ++image->flushes;
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

static void make_name(char path[128], unsigned index)
{
    snprintf(path, 128,
             "/document-%03u-real-world-long-filename-for-directory-test.txt",
             index);
}

static void expect_lookup(struct infs_volume *volume, unsigned index)
{
    char path[128];
    make_name(path, index);
    struct infs_lookup lookup;
    expect(infs_lookup_path(volume, path, &lookup) == INFS_STATUS_OK,
           "lookup entry in paged directory");
    expect(lookup.type == INFS_OBJECT_FILE,
           "paged directory lookup returns file");
}

int main(void)
{
    struct memory_image image = {
        .bytes = calloc(1, TEST_SIZE),
        .size = TEST_SIZE,
        .random_state = UINT64_C(0x9e3779b97f4a7c15),
    };
    expect(image.bytes != NULL, "allocate memory image");

    struct infs_storage storage = make_storage(&image);
    expect(infs_format_storage(&storage, "paged-metadata-test") ==
               INFS_STATUS_OK,
           "format Format 0.9 volume");

    struct infs_volume volume;
    storage = make_storage(&image);
    expect(infs_volume_open_storage(&volume, &storage, 1) == INFS_STATUS_OK,
           "open paged volume writable");
    expect(infs_le16_to_cpu(volume.sb.format_minor) == 9u,
           "Format minor is 0.9");
    expect((infs_le64_to_cpu(volume.sb.incompat_flags) &
            INFS_INCOMPAT_PAGED_METADATA) != 0,
           "paged metadata feature enabled");

    for (unsigned i = 0; i < FILE_COUNT; ++i) {
        char path[128];
        make_name(path, i);
        expect(infs_create_file(&volume, path, NULL) == INFS_STATUS_OK,
               "create file beyond old directory/index limits");
    }

    struct infs_dir_item *items = NULL;
    size_t count = 0;
    expect(infs_list_dir(&volume, "/", &items, &count) == INFS_STATUS_OK,
           "list paged root directory");
    expect(count == FILE_COUNT, "all paged directory entries are listed");
    infs_free_dir_items(items);

    expect_lookup(&volume, 0);
    expect_lookup(&volume, 58);
    expect_lookup(&volume, 126);
    expect_lookup(&volume, FILE_COUNT - 1u);

    char old_path[128];
    make_name(old_path, 150);
    expect(infs_rename(&volume, old_path, "/renamed-entry.txt") ==
               INFS_STATUS_OK,
           "rename entry in paged directory");
    struct infs_lookup lookup;
    expect(infs_lookup_path(&volume, "/renamed-entry.txt", &lookup) ==
               INFS_STATUS_OK,
           "renamed paged entry resolves");
    expect(infs_lookup_path(&volume, old_path, &lookup) == INFS_STATUS_NOT_FOUND,
           "old paged name removed");

    char remove_path[128];
    make_name(remove_path, 80);
    expect(infs_unlink(&volume, remove_path) == INFS_STATUS_OK,
           "remove entry from paged directory");
    expect(infs_lookup_path(&volume, remove_path, &lookup) ==
               INFS_STATUS_NOT_FOUND,
           "removed paged entry stays absent");

    expect(infs_create_file(&volume, "/buffered.bin", NULL) == INFS_STATUS_OK,
           "create buffered-write target");
    expect(infs_volume_set_deferred_publish(
               &volume, 1, UINT64_C(1024) * 1024u) == INFS_STATUS_OK,
           "enable generic deferred publication");
    uint64_t generation_before = infs_le64_to_cpu(volume.sb.generation);
    uint64_t flushes_before = image.flushes;
    uint8_t chunk[512];
    uint8_t expected[8192];
    for (unsigned i = 0; i < 16u; ++i) {
        memset(chunk, (int)(i + 1u), sizeof(chunk));
        memcpy(expected + i * sizeof(chunk), chunk, sizeof(chunk));
        expect(infs_write_file_buffered(&volume, "/buffered.bin", chunk,
                                        sizeof(chunk),
                                        (uint64_t)i * sizeof(chunk)) ==
                   (int64_t)sizeof(chunk),
               "buffered write succeeds");
    }
    expect(volume.tx_active, "buffered writes leave transaction active");
    expect(infs_le64_to_cpu(volume.sb.generation) == generation_before,
           "buffered writes do not publish each chunk");
    expect(image.flushes == flushes_before,
           "buffered writes do not flush each chunk");

    uint8_t readback[sizeof(expected)];
    expect(infs_read_file(&volume, "/buffered.bin", readback,
                          sizeof(readback), 0) == (int64_t)sizeof(readback),
           "read buffered data before publication");
    expect(memcmp(readback, expected, sizeof(expected)) == 0,
           "buffered data is coherent before publication");
    expect(infs_volume_sync(&volume) == INFS_STATUS_OK,
           "publish buffered transaction");
    expect(!volume.tx_active, "sync closes buffered transaction");
    expect(infs_le64_to_cpu(volume.sb.generation) == generation_before + 1u,
           "many buffered writes publish as one generation");

    struct infs_scrub_report report;
    expect(infs_scrub(&volume, &report) == INFS_STATUS_OK,
           "scrub scalable metadata volume");
    expect(report.checksum_errors == 0 && report.metadata_errors == 0,
           "scrub reports scalable metadata clean");
    infs_volume_close(&volume);

    storage = make_storage(&image);
    expect(infs_volume_open_storage(&volume, &storage, 0) == INFS_STATUS_OK,
           "reopen scalable metadata volume");
    items = NULL;
    count = 0;
    expect(infs_list_dir(&volume, "/", &items, &count) == INFS_STATUS_OK,
           "list paged directory after remount");
    expect(count == FILE_COUNT,
           "directory count persists after one rename and one replacement file");
    infs_free_dir_items(items);
    expect(infs_read_file(&volume, "/buffered.bin", readback,
                          sizeof(readback), 0) == (int64_t)sizeof(readback),
           "read buffered file after remount");
    expect(memcmp(readback, expected, sizeof(readback)) == 0,
           "buffered file persists after remount");
    infs_volume_close(&volume);

    free(image.bytes);
    puts("paged metadata and buffered writes: ok");
    return 0;
}
