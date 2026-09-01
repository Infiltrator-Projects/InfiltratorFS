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

#define TEST_BLOCKS UINT64_C(16384)
#define TEST_SIZE ((size_t)(TEST_BLOCKS * INFS_BLOCK_SIZE))
#define CLUSTER_BLOCKS 64u
#define CLUSTER_BYTES ((size_t)CLUSTER_BLOCKS * INFS_BLOCK_SIZE)

struct memory_image {
    uint8_t *bytes;
    size_t size;
    uint64_t random_state;
};

static void fail(const char *message)
{
    fprintf(stderr, "compression-extents: %s\n", message);
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
    *time_ns = INT64_C(1788249600000000000);
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

static struct infs_extent_disk file_first_extent(
    struct infs_volume *vol, struct memory_image *image, const char *path)
{
    struct infs_lookup lookup;
    struct infs_extent_disk result;
    memset(&result, 0, sizeof(result));

    expect(infs_lookup_path(vol, path, &lookup) == INFS_STATUS_OK,
           "lookup extent file");
    uint8_t *object = image->bytes + lookup.block * INFS_BLOCK_SIZE;
    expect(infs_validate_object_block(object), "validate extent file object");

    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)object;
    expect(infs_le16_to_cpu(header->object_type) == INFS_OBJECT_FILE,
           "extent owner is regular file");
    expect(infs_le16_to_cpu(header->object_version) ==
               INFS_OBJECT_VERSION_CLASSIC,
           "qualification file uses classic extent head");

    struct infs_file_payload_disk *file =
        (struct infs_file_payload_disk *)(header + 1);
    expect(infs_le32_to_cpu(file->extent_count) >= 1u,
           "qualification file has extent");
    result = *(struct infs_extent_disk *)(file + 1);
    return result;
}

static uint32_t extent_codec_test(const struct infs_extent_disk *extent)
{
    uint32_t flags = infs_le32_to_cpu(extent->flags);
    return (flags & INFS_EXTENT_CODEC_MASK) >> INFS_EXTENT_CODEC_SHIFT;
}

static uint32_t extent_stored_test(const struct infs_extent_disk *extent)
{
    return infs_le32_to_cpu(extent->flags) >>
        INFS_EXTENT_STORED_BYTES_SHIFT;
}

static void fill_compressible(uint8_t *buffer, size_t size)
{
    static const uint8_t phrase[] =
        "InfiltratorFS adaptive compression generation extent checkpoint ";
    for (size_t i = 0; i < size; ++i)
        buffer[i] = phrase[i % (sizeof(phrase) - 1u)];
}

static void fill_entropy(uint8_t *buffer, size_t size)
{
    uint32_t state = UINT32_C(0x31415926);
    for (size_t i = 0; i < size; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        buffer[i] = (uint8_t)(state >> 24);
    }
}

int main(void)
{
    struct memory_image image = {
        .bytes = calloc(1u, TEST_SIZE),
        .size = TEST_SIZE,
        .random_state = UINT64_C(0x9e3779b97f4a7c15),
    };
    uint8_t *plain = malloc(CLUSTER_BYTES);
    uint8_t *entropy = malloc(CLUSTER_BYTES);
    uint8_t *readback = malloc(CLUSTER_BYTES);
    expect(image.bytes && plain && entropy && readback,
           "allocate qualification buffers");

    struct infs_storage storage = make_storage(&image);
    expect(infs_format_storage(&storage, "compression-test") ==
               INFS_STATUS_OK,
           "format compression volume");

    struct infs_volume vol;
    storage = make_storage(&image);
    expect(infs_volume_open_storage(&vol, &storage, 1) == INFS_STATUS_OK,
           "open compression volume");
    expect((infs_le64_to_cpu(vol.sb.incompat_flags) &
            INFS_INCOMPAT_COMPRESSED_EXTENTS) != 0,
           "compression feature enabled");

    fill_compressible(plain, CLUSTER_BYTES);
    expect(infs_create_file(&vol, "/compressed.bin", NULL) ==
               INFS_STATUS_OK,
           "create compressed file");
    expect(infs_write_file(&vol, "/compressed.bin", plain,
                           CLUSTER_BYTES, 0) ==
               (int64_t)CLUSTER_BYTES,
           "write compressible cluster");

    struct infs_extent_disk compressed =
        file_first_extent(&vol, &image, "/compressed.bin");
    expect(extent_codec_test(&compressed) == INFS_COMPRESSION_IAC1,
           "native IAC1 codec selected");
    expect(extent_stored_test(&compressed) > 0 &&
           extent_stored_test(&compressed) < CLUSTER_BYTES,
           "compressed byte count recorded");

    struct infs_attributes attributes;
    expect(infs_get_attributes(&vol, "/compressed.bin", &attributes) ==
               INFS_STATUS_OK,
           "get compressed allocation");
    expect(attributes.logical_size == CLUSTER_BYTES,
           "compressed logical size preserved");
    expect(attributes.allocated_size < attributes.logical_size,
           "compressed physical allocation is smaller");

    memset(readback, 0, CLUSTER_BYTES);
    expect(infs_read_file(&vol, "/compressed.bin", readback,
                          CLUSTER_BYTES, 0) ==
               (int64_t)CLUSTER_BYTES,
           "read compressed file");
    expect(memcmp(readback, plain, CLUSTER_BYTES) == 0,
           "compressed roundtrip exact");

    fill_entropy(entropy, CLUSTER_BYTES);
    expect(infs_create_file(&vol, "/entropy.bin", NULL) == INFS_STATUS_OK,
           "create entropy file");
    expect(infs_write_file(&vol, "/entropy.bin", entropy,
                           CLUSTER_BYTES, 0) ==
               (int64_t)CLUSTER_BYTES,
           "write entropy cluster");
    struct infs_extent_disk entropy_extent =
        file_first_extent(&vol, &image, "/entropy.bin");
    expect(extent_codec_test(&entropy_extent) == INFS_COMPRESSION_NONE,
           "high entropy falls back to uncompressed extent");

    expect(infs_snapshot_create(&vol, "before-edit") == INFS_STATUS_OK,
           "snapshot compressed generation");

    uint8_t changed[INFS_BLOCK_SIZE];
    memset(changed, 0xa7, sizeof(changed));
    const uint64_t edit_offset = UINT64_C(17) * INFS_BLOCK_SIZE;
    expect(infs_write_file(&vol, "/compressed.bin", changed,
                           sizeof(changed), edit_offset) ==
               (int64_t)sizeof(changed),
           "partial overwrite compressed cluster");

    memset(readback, 0, CLUSTER_BYTES);
    expect(infs_read_file(&vol, "/compressed.bin", readback,
                          CLUSTER_BYTES, 0) ==
               (int64_t)CLUSTER_BYTES,
           "read materialized compressed edit");
    expect(memcmp(readback, plain, (size_t)edit_offset) == 0,
           "bytes before edit preserved");
    expect(memcmp(readback + edit_offset, changed, sizeof(changed)) == 0,
           "edited block preserved");
    expect(memcmp(readback + edit_offset + sizeof(changed),
                  plain + edit_offset + sizeof(changed),
                  CLUSTER_BYTES - (size_t)edit_offset - sizeof(changed)) == 0,
           "bytes after edit preserved");

    memset(readback, 0, CLUSTER_BYTES);
    expect(infs_snapshot_read_file(
               &vol, "before-edit", "/compressed.bin",
               readback, CLUSTER_BYTES, 0) ==
               (int64_t)CLUSTER_BYTES,
           "read snapshot of compressed file");
    expect(memcmp(readback, plain, CLUSTER_BYTES) == 0,
           "snapshot retains original compressed bytes");

    expect(infs_punch_hole(&vol, "/compressed.bin",
                           8u * INFS_BLOCK_SIZE,
                           3u * INFS_BLOCK_SIZE) == INFS_STATUS_OK,
           "hole punch after compressed materialization");
    memset(readback, 0xff, 3u * INFS_BLOCK_SIZE);
    expect(infs_read_file(&vol, "/compressed.bin", readback,
                          3u * INFS_BLOCK_SIZE,
                          8u * INFS_BLOCK_SIZE) ==
               (int64_t)(3u * INFS_BLOCK_SIZE),
           "read punched range");
    for (size_t i = 0; i < 3u * INFS_BLOCK_SIZE; ++i)
        expect(readback[i] == 0, "punched range reads zero");

    expect(infs_truncate_file(
               &vol, "/compressed.bin",
               41u * INFS_BLOCK_SIZE + 123u) == INFS_STATUS_OK,
           "truncate formerly compressed file");

    struct infs_scrub_report report;
    memset(&report, 0, sizeof(report));
    expect(infs_scrub(&vol, &report) == INFS_STATUS_OK,
           "scrub compressed volume");
    expect(report.checksum_errors == 0 && report.metadata_errors == 0,
           "scrub compressed volume clean");

    expect(infs_create_file(&vol, "/corrupt.bin", NULL) == INFS_STATUS_OK,
           "create corruption target");
    expect(infs_write_file(&vol, "/corrupt.bin", plain,
                           CLUSTER_BYTES, 0) ==
               (int64_t)CLUSTER_BYTES,
           "write corruption target");
    struct infs_extent_disk corrupt =
        file_first_extent(&vol, &image, "/corrupt.bin");
    expect(extent_codec_test(&corrupt) == INFS_COMPRESSION_IAC1,
           "corruption target is IAC1");
    uint64_t physical = infs_le64_to_cpu(corrupt.physical_block);
    uint32_t stored = extent_stored_test(&corrupt);
    expect(physical > 0 && stored > 8u, "corruption extent metadata");
    image.bytes[physical * INFS_BLOCK_SIZE + stored / 2u] ^= 0x5au;

    int64_t corrupted_read = infs_read_file(
        &vol, "/corrupt.bin", readback, CLUSTER_BYTES, 0);
    expect(corrupted_read < 0,
           "corrupted compressed payload rejected on read");

    infs_volume_close(&vol);
    free(readback);
    free(entropy);
    free(plain);
    free(image.bytes);
    return 0;
}
