// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/endian.h"
#include "infilfs/format.h"
#include "infilfs/fs.h"
#include "infilfs/storage.h"
#include "infilfs/volume.h"
#include "test-allocation-tree.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_BLOCKS UINT64_C(256)
#define TEST_SIZE ((size_t)(TEST_BLOCKS * INFS_BLOCK_SIZE))

struct memory_image {
    uint8_t *bytes;
    size_t visible_size;
    uint64_t random_state;
};

static void fail(const char *message)
{
    fprintf(stderr, "inline-files: %s\n", message);
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
    if (offset > image->visible_size ||
        size > image->visible_size - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
    memcpy(buffer, image->bytes + (size_t)offset, size);
    return INFS_STATUS_OK;
}

static infs_status memory_write(void *context, uint64_t offset,
                                const void *buffer, size_t size)
{
    struct memory_image *image = context;
    if (offset > image->visible_size ||
        size > image->visible_size - (size_t)offset)
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
    *size_bytes = image->visible_size;
    *is_device = 0;
    return INFS_STATUS_OK;
}

static void memory_close(void *context)
{
    (void)context;
}

static infs_status memory_random(void *context, void *buffer, size_t size)
{
    struct memory_image *image = context;
    uint8_t *bytes = buffer;
    for (size_t i = 0; i < size; ++i) {
        image->random_state ^= image->random_state << 13;
        image->random_state ^= image->random_state >> 7;
        image->random_state ^= image->random_state << 17;
        bytes[i] = (uint8_t)image->random_state;
    }
    return INFS_STATUS_OK;
}

static infs_status memory_time(void *context, int64_t *time_ns)
{
    (void)context;
    *time_ns = INT64_C(1787288400000000000);
    return INFS_STATUS_OK;
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

static void bitmap_set(uint8_t *bitmap, uint64_t block)
{
    bitmap[block >> 3] |= (uint8_t)(1u << (block & 7u));
}

static int id_is_zero_test(const uint8_t id[16])
{
    uint8_t combined = 0;
    for (size_t i = 0; i < 16; ++i)
        combined |= id[i];
    return combined == 0;
}

static void build_valid_image(struct memory_image *image)
{
    static const uint8_t filesystem_id[16] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    };
    static const uint8_t root_id[16] = {
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    };
    static const uint8_t index_id[16] = {
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
        0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    };
    const uint64_t checkpoints[INFS_CHECKPOINT_COUNT] = {0, 128, 255};

    memset(image->bytes, 0, TEST_SIZE);
    image->visible_size = TEST_SIZE;
    image->random_state = UINT64_C(0x9e3779b97f4a7c15);

    const uint64_t initial_objects[] = { UINT64_C(2), UINT64_C(3) };
    test_write_single_leaf_allocation_tree(
        image->bytes, TEST_BLOCKS, 1u, checkpoints,
        initial_objects, sizeof(initial_objects) / sizeof(initial_objects[0]));

    uint8_t block[INFS_BLOCK_SIZE];
    expect(infs_encode_object_index(block, index_id, 1) == INFS_STATUS_OK,
           "encode object index");
    struct infs_object_header_disk *index_header =
        (struct infs_object_header_disk *)block;
    struct infs_index_payload_disk *index_payload =
        (struct infs_index_payload_disk *)(block + sizeof(*index_header));
    struct infs_index_entry_disk *entry =
        (struct infs_index_entry_disk *)(index_payload + 1);
    index_payload->entry_count = infs_cpu_to_le32(1);
    memcpy(entry->object_id, root_id, 16);
    entry->object_block = infs_cpu_to_le64(3);
    entry->object_type = infs_cpu_to_le16(INFS_OBJECT_DIRECTORY);
    index_header->payload_size =
        infs_cpu_to_le32(sizeof(*index_payload) + sizeof(*entry));
    expect(infs_object_finalize(block) == INFS_STATUS_OK,
           "finalize object index");
    memcpy(image->bytes + 2u * INFS_BLOCK_SIZE, block, sizeof(block));

    expect(infs_encode_root_directory(block, root_id, 1, 0755, 0, 0,
                                      INT64_C(1787288400000000000)) ==
               INFS_STATUS_OK,
           "encode root directory");
    memcpy(image->bytes + 3u * INFS_BLOCK_SIZE, block, sizeof(block));

    struct infs_superblock_disk sb;
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, INFS_MAGIC, 8);
    sb.format_major = infs_cpu_to_le16(INFS_FORMAT_MAJOR);
    sb.format_minor = infs_cpu_to_le16(INFS_FORMAT_MINOR);
    sb.header_size = infs_cpu_to_le16(sizeof(sb));
    sb.block_shift = infs_cpu_to_le16(INFS_BLOCK_SHIFT);
    sb.checksum_type = infs_cpu_to_le32(INFS_CHECKSUM_CRC64_ECMA);
    sb.generation = infs_cpu_to_le64(1);
    sb.total_blocks = infs_cpu_to_le64(TEST_BLOCKS);
    sb.free_blocks = infs_cpu_to_le64(TEST_BLOCKS - 9u);
    sb.allocation_root_block = infs_cpu_to_le64(TEST_ALLOCATION_ROOT_BLOCK);
    sb.allocation_leaf_count = infs_cpu_to_le64(1);
    sb.object_index_block = infs_cpu_to_le64(2);
    sb.root_object_block = infs_cpu_to_le64(3);
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        sb.checkpoint_block[i] = infs_cpu_to_le64(checkpoints[i]);
    memcpy(sb.filesystem_uuid, filesystem_id, 16);
    memcpy(sb.root_object_id, root_id, 16);
    sb.incompat_flags = infs_cpu_to_le64(
        INFS_INCOMPAT_UTF8_NAMES | INFS_INCOMPAT_SPARSE_EXTENTS |
        INFS_INCOMPAT_ALLOCATION_TREE |
        INFS_INCOMPAT_INLINE_DATA);
    memcpy(sb.label, "Inline test", 11);
    expect(infs_encode_superblock(block, &sb) == INFS_STATUS_OK,
           "encode checkpoint");
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        memcpy(image->bytes + checkpoints[i] * INFS_BLOCK_SIZE,
               block, sizeof(block));
}

static void expect_inline(struct infs_volume *volume,
                          struct memory_image *image, uint64_t expected_size)
{
    struct infs_lookup lookup;
    expect(infs_lookup_path(volume, "/tiny", &lookup) == INFS_STATUS_OK,
           "lookup inline file");
    uint8_t *block = image->bytes + lookup.block * INFS_BLOCK_SIZE;
    expect(infs_validate_object_block(block), "inline object CRC");
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)block;
    struct infs_file_payload_disk *file =
        (struct infs_file_payload_disk *)(block + sizeof(*header));
    expect(infs_le32_to_cpu(file->extent_count) == 0,
           "inline file must have no extents");
    expect(id_is_zero_test(file->checksum_head_id),
           "inline file must have no checksum chain");
    expect(infs_le64_to_cpu(file->attributes.logical_size) == expected_size,
           "inline logical size");
    if (expected_size == 0) {
        expect(infs_le32_to_cpu(header->payload_size) == sizeof(*file),
               "empty inline payload shape");
    } else {
        expect(infs_le32_to_cpu(header->payload_size) ==
                   sizeof(*file) + sizeof(struct infs_data_checksum_disk) +
                       expected_size,
               "inline payload shape");
    }
}

static void expect_external(struct infs_volume *volume,
                            struct memory_image *image)
{
    struct infs_lookup lookup;
    expect(infs_lookup_path(volume, "/tiny", &lookup) == INFS_STATUS_OK,
           "lookup promoted file");
    uint8_t *block = image->bytes + lookup.block * INFS_BLOCK_SIZE;
    expect(infs_validate_object_block(block), "promoted object CRC");
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)block;
    struct infs_file_payload_disk *file =
        (struct infs_file_payload_disk *)(block + sizeof(*header));
    expect(infs_le32_to_cpu(file->extent_count) != 0,
           "promoted file must have extents");
    expect(!id_is_zero_test(file->checksum_head_id),
           "promoted file must have checksum chain");
}

int main(void)
{
    struct memory_image image = {0};
    image.bytes = calloc(1, TEST_SIZE);
    expect(image.bytes != NULL, "allocate memory image");
    build_valid_image(&image);

    struct infs_storage storage = make_storage(&image);
    struct infs_volume volume;
    expect(infs_volume_open_storage(&volume, &storage, 1) == INFS_STATUS_OK,
           "open writable test volume");
    expect(infs_create_file(&volume, "/tiny", NULL) == INFS_STATUS_OK,
           "create tiny file");
    expect_inline(&volume, &image, 0);

    uint8_t first[100];
    for (size_t i = 0; i < sizeof(first); ++i)
        first[i] = (uint8_t)(i + 1u);
    expect(infs_write_file(&volume, "/tiny", first, sizeof(first), 0) ==
               (int64_t)sizeof(first),
           "write tiny inline data");

    struct infs_attributes attributes;
    expect(infs_get_attributes(&volume, "/tiny", &attributes) == INFS_STATUS_OK,
           "get inline attributes");
    expect(attributes.logical_size == sizeof(first), "tiny logical size");
    expect(attributes.allocated_size == 0, "tiny file uses no data blocks");
    expect_inline(&volume, &image, sizeof(first));

    const uint8_t marker = 0xa5;
    expect(infs_write_file(&volume, "/tiny", &marker, 1, 3000) == 1,
           "write high inline offset");
    expect_inline(&volume, &image, 3001);
    uint8_t *readback = calloc(1, INFS_INLINE_DATA_MAX + 1u);
    expect(readback != NULL, "allocate readback");
    expect(infs_read_file(&volume, "/tiny", readback, 3001, 0) == 3001,
           "read sparse inline content");
    expect(memcmp(readback, first, sizeof(first)) == 0,
           "inline prefix preserved");
    for (size_t i = sizeof(first); i < 3000; ++i)
        expect(readback[i] == 0, "inline unwritten gap is zero");
    expect(readback[3000] == marker, "inline high-offset marker");

    expect(infs_punch_hole(&volume, "/tiny", 20, 60) == INFS_STATUS_OK,
           "punch inline range");
    memset(readback, 0xff, 100);
    expect(infs_read_file(&volume, "/tiny", readback, 100, 0) == 100,
           "read punched inline prefix");
    expect(memcmp(readback, first, 20) == 0, "punch preserves prefix");
    for (size_t i = 20; i < 80; ++i)
        expect(readback[i] == 0, "punched inline bytes are zero");
    expect(memcmp(readback + 80, first + 80, 20) == 0,
           "punch preserves suffix");

    expect(infs_truncate_file(&volume, "/tiny", INFS_INLINE_DATA_MAX) ==
               INFS_STATUS_OK,
           "grow to inline limit");
    expect(infs_get_attributes(&volume, "/tiny", &attributes) == INFS_STATUS_OK,
           "attributes at inline limit");
    expect(attributes.logical_size == INFS_INLINE_DATA_MAX,
           "logical size at inline limit");
    expect(attributes.allocated_size == 0,
           "inline limit still uses no data blocks");
    expect_inline(&volume, &image, INFS_INLINE_DATA_MAX);

    const uint8_t boundary = 0x5a;
    expect(infs_write_file(&volume, "/tiny", &boundary, 1,
                           INFS_INLINE_DATA_MAX) == 1,
           "promote beyond inline limit");
    expect(infs_get_attributes(&volume, "/tiny", &attributes) == INFS_STATUS_OK,
           "attributes after promotion");
    expect(attributes.logical_size == INFS_INLINE_DATA_MAX + 1u,
           "promoted logical size");
    expect(attributes.allocated_size == INFS_BLOCK_SIZE,
           "promoted file owns one data block");
    expect_external(&volume, &image);
    expect(infs_read_file(&volume, "/tiny", readback, 1,
                          INFS_INLINE_DATA_MAX) == 1,
           "read promoted boundary byte");
    expect(readback[0] == boundary, "promoted boundary value");

    expect(infs_truncate_file(&volume, "/tiny", 128) == INFS_STATUS_OK,
           "fold promoted file back inline");
    expect(infs_get_attributes(&volume, "/tiny", &attributes) == INFS_STATUS_OK,
           "attributes after fold inline");
    expect(attributes.logical_size == 128, "folded logical size");
    expect(attributes.allocated_size == 0,
           "folded inline file releases data block");
    expect_inline(&volume, &image, 128);

    struct infs_scrub_report report;
    expect(infs_scrub(&volume, &report) == INFS_STATUS_OK,
           "scrub inline file");
    expect(report.files_checked == 1, "scrub sees inline file");
    expect(report.data_blocks_checked == 1,
           "scrub verifies inline logical data block");
    expect(report.checksum_errors == 0 && report.metadata_errors == 0,
           "inline scrub clean");

    infs_volume_close(&volume);
    storage = make_storage(&image);
    expect(infs_volume_open_storage(&volume, &storage, 0) == INFS_STATUS_OK,
           "reopen inline volume read-only");
    memset(readback, 0xcc, 128);
    expect(infs_read_file(&volume, "/tiny", readback, 128, 0) == 128,
           "read folded inline file after reopen");
    expect(memcmp(readback, first, 20) == 0, "reopen preserves prefix");
    for (size_t i = 20; i < 80; ++i)
        expect(readback[i] == 0, "reopen preserves punched zeros");
    expect(memcmp(readback + 80, first + 80, 20) == 0,
           "reopen preserves suffix");
    for (size_t i = 100; i < 128; ++i)
        expect(readback[i] == 0, "reopen preserves zero growth");
    infs_volume_close(&volume);

    free(readback);
    free(image.bytes);
    puts("inline-files: ok");
    return 0;
}
