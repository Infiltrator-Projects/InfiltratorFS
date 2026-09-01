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

#define TEST_BLOCKS UINT64_C(8192)
#define TEST_SIZE ((size_t)(TEST_BLOCKS * INFS_BLOCK_SIZE))
#define COPY_CHUNK_SIZE INFS_BLOCK_SIZE
#define TEST_FILE_SIZE (UINT64_C(192) * COPY_CHUNK_SIZE)
#define FRAGMENTED_LOGICAL_BLOCKS 384u
#define FRAGMENTED_WRITTEN_BLOCKS 100u
#define RANDOM_LOGICAL_BLOCKS UINT64_C(131072)
#define RANDOM_WRITTEN_BLOCKS 320u
#define RANDOM_SYNC_INDEX 256u

struct memory_image {
    uint8_t *bytes;
    size_t size;
    uint64_t random_state;
};

static void fail(const char *message)
{
    fprintf(stderr, "large-files: %s\n", message);
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
    *time_ns = INT64_C(1787547600000000000);
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

static uint8_t expected_byte(uint64_t offset)
{
    uint64_t mixed = offset * UINT64_C(0x9e3779b97f4a7c15);
    mixed ^= mixed >> 29;
    mixed ^= offset >> 7;
    return (uint8_t)mixed;
}

static void fill_pattern(uint8_t *buffer, size_t size, uint64_t offset)
{
    for (size_t i = 0; i < size; ++i)
        buffer[i] = expected_byte(offset + i);
}

static uint64_t scattered_logical_block(uint64_t sequence)
{
    /* An odd multiplier modulo 2^17 gives a permutation, so the first 320
     * writes are unique but deliberately jump around the 512 MiB sparse file.
     * This reproduces the non-tail extent-page edits seen by the Hyper-V
     * mounted stress test without depending on libc/Python RNG behaviour. */
    return (sequence * UINT64_C(40503) + UINT64_C(7919)) &
           (RANDOM_LOGICAL_BLOCKS - 1u);
}

static void verify_range(struct infs_volume *volume, uint64_t offset)
{
    uint8_t actual[INFS_BLOCK_SIZE];
    uint8_t expected[INFS_BLOCK_SIZE];
    fill_pattern(expected, sizeof(expected), offset);
    expect(infs_read_file(volume, "/movie.bin", actual, sizeof(actual),
                          offset) == (int64_t)sizeof(actual),
           "read large-file probe");
    expect(memcmp(actual, expected, sizeof(actual)) == 0,
           "large-file probe data");
}

static void verify_fragmented_data(struct infs_volume *volume,
                                   uint64_t logical_block)
{
    uint8_t actual[INFS_BLOCK_SIZE];
    uint8_t expected[INFS_BLOCK_SIZE];
    uint64_t offset = logical_block * INFS_BLOCK_SIZE;
    fill_pattern(expected, sizeof(expected), offset);
    expect(infs_read_file(volume, "/fragmented.bin", actual, sizeof(actual),
                          offset) == (int64_t)sizeof(actual),
           "read fragmented data block");
    expect(memcmp(actual, expected, sizeof(actual)) == 0,
           "fragmented data block contents");
}

static void verify_fragmented_hole(struct infs_volume *volume,
                                   uint64_t logical_block)
{
    uint8_t actual[INFS_BLOCK_SIZE];
    uint8_t zero[INFS_BLOCK_SIZE] = {0};
    expect(infs_read_file(volume, "/fragmented.bin", actual, sizeof(actual),
                          logical_block * INFS_BLOCK_SIZE) ==
               (int64_t)sizeof(actual),
           "read fragmented hole block");
    expect(memcmp(actual, zero, sizeof(actual)) == 0,
           "fragmented hole reads as zero");
}

static void verify_random_data(struct infs_volume *volume, uint64_t sequence)
{
    uint8_t actual[INFS_BLOCK_SIZE];
    uint8_t expected[INFS_BLOCK_SIZE];
    uint64_t logical = scattered_logical_block(sequence);
    uint64_t offset = logical * INFS_BLOCK_SIZE;
    fill_pattern(expected, sizeof(expected), offset);
    expect(infs_read_file(volume, "/random.bin", actual, sizeof(actual),
                          offset) == (int64_t)sizeof(actual),
           "read scattered overwrite block");
    expect(memcmp(actual, expected, sizeof(actual)) == 0,
           "scattered overwrite contents");
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
    expect(infs_format_storage(&storage, "large-file-test") ==
               INFS_STATUS_OK,
           "format large-file volume");

    struct infs_volume volume;
    storage = make_storage(&image);
    expect(infs_volume_open_storage(&volume, &storage, 1) == INFS_STATUS_OK,
           "open writable volume");
    expect(infs_le16_to_cpu(volume.sb.format_minor) == INFS_FORMAT_MINOR,
           "current Format minor");
    expect((infs_le64_to_cpu(volume.sb.incompat_flags) &
            INFS_INCOMPAT_PAGED_EXTENTS) != 0,
           "paged extent feature enabled");
    expect((infs_le64_to_cpu(volume.sb.incompat_flags) &
            INFS_INCOMPAT_COMPRESSED_EXTENTS) == 0,
           "experimental compression is not enabled by normal formatting");
    expect(infs_create_file(&volume, "/movie.bin", NULL) == INFS_STATUS_OK,
           "create large file");

    uint8_t *chunk = malloc(COPY_CHUNK_SIZE);
    expect(chunk != NULL, "allocate copy buffer");
    for (uint64_t offset = 0; offset < TEST_FILE_SIZE;
         offset += COPY_CHUNK_SIZE) {
        fill_pattern(chunk, COPY_CHUNK_SIZE, offset);
        expect(infs_write_file(&volume, "/movie.bin", chunk,
                               COPY_CHUNK_SIZE, offset) ==
                   (int64_t)COPY_CHUNK_SIZE,
               "durable sequential copy write");
    }
    free(chunk);
    expect(volume.checksum_cursor_hits > 180u,
           "sequential checksum cursor used");
    expect(volume.checksum_chain_steps <= 200u,
           "sequential checksum traversal remains linear");

    struct infs_attributes attributes;
    expect(infs_get_attributes(&volume, "/movie.bin", &attributes) ==
               INFS_STATUS_OK &&
               attributes.logical_size == TEST_FILE_SIZE &&
               attributes.allocated_size == TEST_FILE_SIZE,
           "large-file size and allocation accounting");

    struct infs_lookup lookup;
    expect(infs_lookup_path(&volume, "/movie.bin", &lookup) == INFS_STATUS_OK,
           "lookup large file");
    uint8_t *object = image.bytes + lookup.block * INFS_BLOCK_SIZE;
    expect(infs_validate_object_block(object), "validate large-file object");
    struct infs_file_payload_disk *file =
        (struct infs_file_payload_disk *)(object +
            sizeof(struct infs_object_header_disk));
    expect(infs_le32_to_cpu(file->extent_count) <= 2u,
           "sequential copy remains physically compact");

    verify_range(&volume, 0);
    verify_range(&volume, TEST_FILE_SIZE / 2u);
    verify_range(&volume, TEST_FILE_SIZE - INFS_BLOCK_SIZE);

    /* Force more than the old single-object extent ceiling. Alternating data
     * and holes creates over 161 logical extents while retaining only 100
     * allocated data blocks. Current format must promote the extent vector to
     * checksummed paged metadata and preserve it through scrub/remount. */
    expect(infs_create_file(&volume, "/fragmented.bin", NULL) == INFS_STATUS_OK,
           "create fragmented file");
    expect(infs_truncate_file(
               &volume, "/fragmented.bin",
               (uint64_t)FRAGMENTED_LOGICAL_BLOCKS * INFS_BLOCK_SIZE) ==
               INFS_STATUS_OK,
           "create sparse fragmented logical range");
    expect(infs_volume_set_deferred_publish(
               &volume, 1, UINT64_C(64) * 1024u * 1024u) == INFS_STATUS_OK,
           "enable deferred publication for fragmentation workload");

    uint8_t fragmented_block[INFS_BLOCK_SIZE];
    for (uint64_t i = 0; i < FRAGMENTED_WRITTEN_BLOCKS; ++i) {
        uint64_t logical = i * 2u;
        uint64_t offset = logical * INFS_BLOCK_SIZE;
        fill_pattern(fragmented_block, sizeof(fragmented_block), offset);
        expect(infs_write_file_buffered(
                   &volume, "/fragmented.bin", fragmented_block,
                   sizeof(fragmented_block), offset) ==
                   (int64_t)sizeof(fragmented_block),
               "write alternating fragmented data block");
    }
    expect(infs_volume_sync(&volume) == INFS_STATUS_OK,
           "publish fragmented file transaction");

    expect(infs_lookup_path(&volume, "/fragmented.bin", &lookup) ==
               INFS_STATUS_OK,
           "lookup fragmented file");
    object = image.bytes + lookup.block * INFS_BLOCK_SIZE;
    expect(infs_validate_object_block(object), "validate fragmented object");
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)object;
    file = (struct infs_file_payload_disk *)(header + 1);
    expect(infs_le16_to_cpu(header->object_version) == INFS_OBJECT_VERSION_PAGED,
           "fragmented file promoted to paged extents");
    expect(infs_le32_to_cpu(file->extent_count) > 161u,
           "fragmented file exceeds legacy inline extent ceiling");

    expect(infs_get_attributes(&volume, "/fragmented.bin", &attributes) ==
               INFS_STATUS_OK &&
               attributes.logical_size ==
                   (uint64_t)FRAGMENTED_LOGICAL_BLOCKS * INFS_BLOCK_SIZE &&
               attributes.allocated_size ==
                   (uint64_t)FRAGMENTED_WRITTEN_BLOCKS * INFS_BLOCK_SIZE,
           "paged extent size and allocation accounting");
    verify_fragmented_data(&volume, 0u);
    verify_fragmented_hole(&volume, 1u);
    verify_fragmented_data(&volume, 98u);
    verify_fragmented_hole(&volume, 99u);
    verify_fragmented_data(&volume, 198u);
    verify_fragmented_hole(&volume, 199u);
    verify_fragmented_hole(&volume, FRAGMENTED_LOGICAL_BLOCKS - 1u);

    /* Reproduce the mounted Hyper-V failure: start with a 512 MiB sparse file,
     * then make small full-block writes at non-monotonic locations.  The first
     * explicit sync deliberately lands after the classic->paged promotion and
     * several non-tail page edits.  Both the sync point and final state must be
     * independently scrub-clean and reopenable. */
    expect(infs_create_file(&volume, "/random.bin", NULL) == INFS_STATUS_OK,
           "create scattered overwrite file");
    expect(infs_truncate_file(
               &volume, "/random.bin",
               RANDOM_LOGICAL_BLOCKS * INFS_BLOCK_SIZE) == INFS_STATUS_OK,
           "create 512 MiB sparse random-overwrite file");

    uint8_t random_block[INFS_BLOCK_SIZE];
    struct infs_scrub_report midpoint_report;
    for (uint64_t i = 0; i < RANDOM_WRITTEN_BLOCKS; ++i) {
        uint64_t logical = scattered_logical_block(i);
        uint64_t offset = logical * INFS_BLOCK_SIZE;
        fill_pattern(random_block, sizeof(random_block), offset);
        expect(infs_write_file_buffered(
                   &volume, "/random.bin", random_block,
                   sizeof(random_block), offset) ==
                   (int64_t)sizeof(random_block),
               "write scattered full data block");
        if (i == RANDOM_SYNC_INDEX) {
            expect(infs_volume_sync(&volume) == INFS_STATUS_OK,
                   "publish scattered overwrite midpoint");
            expect(infs_scrub(&volume, &midpoint_report) == INFS_STATUS_OK &&
                       midpoint_report.checksum_errors == 0 &&
                       midpoint_report.metadata_errors == 0,
                   "midpoint scattered overwrite scrub clean");
        }
    }
    expect(infs_volume_sync(&volume) == INFS_STATUS_OK,
           "publish scattered overwrite transaction");
    expect(infs_get_attributes(&volume, "/random.bin", &attributes) ==
               INFS_STATUS_OK &&
               attributes.logical_size ==
                   RANDOM_LOGICAL_BLOCKS * INFS_BLOCK_SIZE &&
               attributes.allocated_size ==
                   (uint64_t)RANDOM_WRITTEN_BLOCKS * INFS_BLOCK_SIZE,
           "scattered overwrite size and allocation accounting");
    verify_random_data(&volume, 0u);
    verify_random_data(&volume, 80u);
    verify_random_data(&volume, 160u);
    verify_random_data(&volume, RANDOM_SYNC_INDEX);
    verify_random_data(&volume, RANDOM_WRITTEN_BLOCKS - 1u);

    struct infs_scrub_report report;
    expect(infs_scrub(&volume, &report) == INFS_STATUS_OK &&
               report.files_checked == 3u &&
               report.data_blocks_checked ==
                   TEST_FILE_SIZE / INFS_BLOCK_SIZE +
                   FRAGMENTED_WRITTEN_BLOCKS + RANDOM_WRITTEN_BLOCKS &&
               report.checksum_errors == 0 && report.metadata_errors == 0,
           "scrub compact, sequential-fragmented and random-paged files");
    infs_volume_close(&volume);

    storage = make_storage(&image);
    expect(infs_volume_open_storage(&volume, &storage, 0) == INFS_STATUS_OK,
           "reopen large-file volume");
    verify_range(&volume, TEST_FILE_SIZE - INFS_BLOCK_SIZE);
    verify_fragmented_data(&volume, 198u);
    verify_fragmented_hole(&volume, 199u);
    verify_random_data(&volume, 0u);
    verify_random_data(&volume, RANDOM_SYNC_INDEX);
    verify_random_data(&volume, RANDOM_WRITTEN_BLOCKS - 1u);
    infs_volume_close(&volume);

    free(image.bytes);
    puts("large-file and paged-extent conformance: ok");
    return 0;
}
