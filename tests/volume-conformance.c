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

#define TEST_BLOCKS UINT64_C(4096)
#define TEST_SIZE ((size_t)(TEST_BLOCKS * INFS_BLOCK_SIZE))

struct memory_image {
    uint8_t *bytes;
    size_t visible_size;
    int fail_reads;
    uint64_t random_state;
};

static void fail(const char *message)
{
    fprintf(stderr, "volume-conformance: %s\n", message);
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
    if (image->fail_reads)
        return INFS_STATUS_IO_ERROR;
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
    *time_ns = INT64_C(1786744800000000000);
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

static void bitmap_set(uint8_t *bitmap, uint64_t block, int allocated)
{
    uint8_t mask = (uint8_t)(1u << (block & 7u));
    if (allocated)
        bitmap[block >> 3] |= mask;
    else
        bitmap[block >> 3] &= (uint8_t)~mask;
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
    const uint64_t checkpoints[INFS_CHECKPOINT_COUNT] = {0, 2048, 4095};

    memset(image->bytes, 0, TEST_SIZE);
    image->visible_size = TEST_SIZE;
    image->fail_reads = 0;
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
                                      INT64_C(1786744800000000000)) ==
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
        INFS_INCOMPAT_ALLOCATION_TREE);
    memcpy(sb.label, "Conformance", 11);
    expect(infs_encode_superblock(block, &sb) == INFS_STATUS_OK,
           "encode checkpoint");
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        memcpy(image->bytes + checkpoints[i] * INFS_BLOCK_SIZE,
               block, sizeof(block));
}

static int open_image(struct memory_image *image)
{
    struct infs_storage storage = make_storage(image);
    struct infs_volume volume;
    int status = infs_volume_open_storage(&volume, &storage, 0);
    if (status == 0)
        infs_volume_close(&volume);
    return status;
}

static void rewrite_checkpoints(struct memory_image *image,
                                struct infs_superblock_disk *sb)
{
    uint8_t block[INFS_BLOCK_SIZE];
    const uint64_t checkpoints[INFS_CHECKPOINT_COUNT] = {0, 2048, 4095};
    expect(infs_encode_superblock(block, sb) == INFS_STATUS_OK,
           "rewrite malformed checkpoint");
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        memcpy(image->bytes + checkpoints[i] * INFS_BLOCK_SIZE,
               block, sizeof(block));
}

static struct infs_superblock_disk current_superblock(struct memory_image *image)
{
    struct infs_superblock_disk sb;
    memcpy(&sb, image->bytes, sizeof(sb));
    return sb;
}

static void make_invalid_utf8_root(struct memory_image *image)
{
    uint8_t *block = image->bytes + 3u * INFS_BLOCK_SIZE;
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)block;
    struct infs_directory_payload_disk *payload =
        (struct infs_directory_payload_disk *)(block + sizeof(*header));
    struct infs_dirent_disk *entry =
        (struct infs_dirent_disk *)(payload + 1);
    memset(entry, 0, 32);
    entry->record_size = infs_cpu_to_le16(32);
    entry->name_length = infs_cpu_to_le16(2);
    entry->object_type = infs_cpu_to_le16(INFS_OBJECT_FILE);
    entry->object_id[0] = 0x55;
    uint8_t *name = (uint8_t *)(entry + 1);
    name[0] = 0xc0;
    name[1] = 0x80;
    payload->entry_count = infs_cpu_to_le32(1);
    payload->bytes_used = infs_cpu_to_le32(32);
    header->payload_size = infs_cpu_to_le32(sizeof(*payload) + 32u);
    expect(infs_object_finalize(block) == INFS_STATUS_OK,
           "finalize malformed UTF-8 root");
}

static void check_sparse_files(struct memory_image *image)
{
    static const uint8_t zero_id[16] = {0};
    const uint64_t sparse_size = (UINT64_C(1) << 40) + 123u;
    const uint64_t data_offset = (UINT64_C(1) << 40) -
        (2u * INFS_BLOCK_SIZE) + 77u;
    const uint64_t data_block_start =
        (data_offset / INFS_BLOCK_SIZE) * INFS_BLOCK_SIZE;
    const uint64_t middle_offset = (UINT64_C(1) << 39) + 33u;
    const uint64_t middle_block_start =
        (middle_offset / INFS_BLOCK_SIZE) * INFS_BLOCK_SIZE;
    static const uint8_t payload[] = "SPARSE-DATA";
    static const uint8_t low_payload[] = "LOW";
    static const uint8_t middle_payload[] = "MIDDLE";
    const struct infs_create_options options = {
        .posix_permissions = 0644,
        .posix_uid = 1000,
        .posix_gid = 1000,
    };

    build_valid_image(image);
    struct infs_storage storage = make_storage(image);
    struct infs_volume volume;
    expect(infs_volume_open_storage(&volume, &storage, 1) == 0,
           "open writable sparse test image");
    expect(infs_create_file(&volume, "/sparse", &options) == 0,
           "create sparse file");
    uint64_t free_after_create = infs_le64_to_cpu(volume.sb.free_blocks);

    expect(infs_truncate_file(&volume, "/sparse", sparse_size) == 0,
           "grow sparse file without allocation");
    struct infs_attributes attributes;
    expect(infs_get_attributes(&volume, "/sparse", &attributes) == 0 &&
               attributes.logical_size == sparse_size &&
               attributes.allocated_size == 0,
           "report sparse logical and allocated sizes");
    expect(infs_le64_to_cpu(volume.sb.free_blocks) == free_after_create,
           "sparse growth consumes no blocks");

    expect(infs_write_file(&volume, "/sparse", payload,
                           sizeof(payload) - 1u, data_offset) ==
               (int64_t)(sizeof(payload) - 1u),
           "write one high-offset sparse block");
    expect(infs_get_attributes(&volume, "/sparse", &attributes) == 0 &&
               attributes.logical_size == sparse_size &&
               attributes.allocated_size == INFS_BLOCK_SIZE,
           "allocate only the written sparse block");

    uint8_t probe[INFS_BLOCK_SIZE];
    expect(infs_read_file(&volume, "/sparse", probe, sizeof(probe),
                          data_block_start) == (int64_t)sizeof(probe),
           "read sparse data block");
    for (size_t i = 0; i < sizeof(probe); ++i) {
        uint8_t expected = 0;
        if (i >= 77u && i < 77u + sizeof(payload) - 1u)
            expected = payload[i - 77u];
        expect(probe[i] == expected, "holes around sparse write read as zero");
    }

    /* Create checksum segments out of logical order: the high write created
     * the head, then the low and middle writes must insert before and within
     * the sorted sparse checksum chain. */
    expect(infs_write_file(&volume, "/sparse", low_payload,
                           sizeof(low_payload) - 1u, 17u) ==
               (int64_t)(sizeof(low_payload) - 1u),
           "insert low sparse checksum segment");
    expect(infs_write_file(&volume, "/sparse", middle_payload,
                           sizeof(middle_payload) - 1u, middle_offset) ==
               (int64_t)(sizeof(middle_payload) - 1u),
           "insert middle sparse checksum segment");
    expect(infs_get_attributes(&volume, "/sparse", &attributes) == 0 &&
               attributes.allocated_size == 3u * INFS_BLOCK_SIZE,
           "track three discontiguous sparse blocks");

    struct infs_scrub_report report;
    expect(infs_scrub(&volume, &report) == 0 &&
               report.files_checked == 1 &&
               report.data_blocks_checked == 3 &&
               report.checksum_errors == 0 && report.metadata_errors == 0,
           "scrub out-of-order sparse checksum segments");

    expect(infs_punch_hole(&volume, "/sparse", data_offset + 2u, 3u) == 0,
           "punch partial sparse block");
    uint8_t expected_payload[sizeof(payload) - 1u];
    memcpy(expected_payload, payload, sizeof(expected_payload));
    memset(expected_payload + 2u, 0, 3u);
    uint8_t payload_readback[sizeof(payload) - 1u];
    expect(infs_read_file(&volume, "/sparse", payload_readback,
                          sizeof(payload_readback), data_offset) ==
               (int64_t)sizeof(payload_readback) &&
               memcmp(payload_readback, expected_payload,
                      sizeof(payload_readback)) == 0,
           "partial punch preserves surrounding bytes");
    expect(infs_get_attributes(&volume, "/sparse", &attributes) == 0 &&
               attributes.allocated_size == 3u * INFS_BLOCK_SIZE,
           "partial punch retains the data block");

    expect(infs_punch_hole(&volume, "/sparse", data_block_start,
                           INFS_BLOCK_SIZE) == 0,
           "punch complete sparse block");
    expect(infs_punch_hole(&volume, "/sparse", 0, INFS_BLOCK_SIZE) == 0,
           "punch low sparse block");
    expect(infs_punch_hole(&volume, "/sparse", middle_block_start,
                           INFS_BLOCK_SIZE) == 0,
           "punch middle sparse block");
    expect(infs_get_attributes(&volume, "/sparse", &attributes) == 0 &&
               attributes.logical_size == sparse_size &&
               attributes.allocated_size == 0,
           "full punch reclaims allocation and keeps size");
    expect(infs_le64_to_cpu(volume.sb.free_blocks) == free_after_create,
           "full punch reclaims data and checksum blocks");
    expect(infs_scrub(&volume, &report) == 0 &&
               report.data_blocks_checked == 0 &&
               report.checksum_errors == 0 && report.metadata_errors == 0,
           "scrub skips fully punched holes");

    struct infs_lookup lookup;
    expect(infs_lookup_path(&volume, "/sparse", &lookup) == 0,
           "locate punched sparse object");
    uint8_t *object = image->bytes + lookup.block * INFS_BLOCK_SIZE;
    struct infs_file_payload_disk *file =
        (struct infs_file_payload_disk *)(object +
            sizeof(struct infs_object_header_disk));
    struct infs_extent_disk *extent = (struct infs_extent_disk *)(file + 1);
    expect(infs_validate_object_block(object) &&
               infs_le32_to_cpu(file->extent_count) == 1 &&
               infs_le32_to_cpu(extent->flags) == INFS_EXTENT_HOLE &&
               infs_le64_to_cpu(extent->physical_block) == 0 &&
               memcmp(file->checksum_head_id, zero_id, 16) == 0,
           "persist one hole extent and no checksum chain");
    infs_volume_close(&volume);

    storage = make_storage(image);
    expect(infs_volume_open_storage(&volume, &storage, 0) == 0,
           "reopen sparse image read-only");
    memset(payload_readback, 0xff, sizeof(payload_readback));
    expect(infs_read_file(&volume, "/sparse", payload_readback,
                          sizeof(payload_readback), data_offset) ==
               (int64_t)sizeof(payload_readback),
           "read fully punched bytes after reopen");
    for (size_t i = 0; i < sizeof(payload_readback); ++i)
        expect(payload_readback[i] == 0,
               "fully punched bytes remain zero after reopen");
    infs_volume_close(&volume);

    uint8_t *valid_sparse_image = malloc(TEST_SIZE);
    expect(valid_sparse_image != NULL, "allocate sparse corruption snapshot");
    memcpy(valid_sparse_image, image->bytes, TEST_SIZE);

    object = image->bytes + lookup.block * INFS_BLOCK_SIZE;
    file = (struct infs_file_payload_disk *)(object +
        sizeof(struct infs_object_header_disk));
    extent = (struct infs_extent_disk *)(file + 1);
    extent->flags = infs_cpu_to_le32(UINT32_C(0x80000000));
    expect(infs_object_finalize(object) == 0 && open_image(image) != 0,
           "reject unknown sparse extent flags");

    memcpy(image->bytes, valid_sparse_image, TEST_SIZE);
    object = image->bytes + lookup.block * INFS_BLOCK_SIZE;
    file = (struct infs_file_payload_disk *)(object +
        sizeof(struct infs_object_header_disk));
    extent = (struct infs_extent_disk *)(file + 1);
    extent->physical_block = infs_cpu_to_le64(1);
    expect(infs_object_finalize(object) == 0 && open_image(image) != 0,
           "reject hole extent with physical storage");

    memcpy(image->bytes, valid_sparse_image, TEST_SIZE);
    object = image->bytes + lookup.block * INFS_BLOCK_SIZE;
    file = (struct infs_file_payload_disk *)(object +
        sizeof(struct infs_object_header_disk));
    extent = (struct infs_extent_disk *)(file + 1);
    extent->block_count = infs_cpu_to_le32(
        infs_le32_to_cpu(extent->block_count) - 1u);
    expect(infs_object_finalize(object) == 0 && open_image(image) != 0,
           "reject sparse extent coverage mismatch");

    memcpy(image->bytes, valid_sparse_image, TEST_SIZE);
    free(valid_sparse_image);
}

int main(void)
{
    struct memory_image image = {
        .bytes = malloc(TEST_SIZE),
        .visible_size = TEST_SIZE,
    };
    if (!image.bytes)
        fail("allocate test image");

    build_valid_image(&image);
    expect(open_image(&image) == 0, "accept deterministic valid image");

    check_sparse_files(&image);

    build_valid_image(&image);
    image.bytes[100] ^= 1u;
    expect(open_image(&image) == 0, "survive one damaged checkpoint copy");

    build_valid_image(&image);
    image.bytes[100] ^= 1u;
    image.bytes[2048u * INFS_BLOCK_SIZE + 100u] ^= 1u;
    image.bytes[4095u * INFS_BLOCK_SIZE + 100u] ^= 1u;
    expect(open_image(&image) != 0, "reject all damaged checkpoints");

    build_valid_image(&image);
    struct infs_superblock_disk sb = current_superblock(&image);
    sb.incompat_flags = infs_cpu_to_le64(
        INFS_INCOMPAT_UTF8_NAMES | INFS_INCOMPAT_SPARSE_EXTENTS |
        (UINT64_C(1) << 63));
    rewrite_checkpoints(&image, &sb);
    expect(open_image(&image) != 0, "reject unknown incompatible feature");

    build_valid_image(&image);
    sb = current_superblock(&image);
    sb.free_blocks = infs_cpu_to_le64(TEST_BLOCKS - 7u);
    rewrite_checkpoints(&image, &sb);
    expect(open_image(&image) != 0, "reject allocation accounting mismatch");

    build_valid_image(&image);
    bitmap_set(image.bytes + INFS_BLOCK_SIZE, 3, 0);
    expect(open_image(&image) != 0, "reject free root block");

    build_valid_image(&image);
    sb = current_superblock(&image);
    sb.root_object_id[0] ^= 0xffu;
    rewrite_checkpoints(&image, &sb);
    expect(open_image(&image) != 0, "reject root identity mismatch");

    build_valid_image(&image);
    make_invalid_utf8_root(&image);
    expect(open_image(&image) != 0, "reject malformed UTF-8 directory entry");

    build_valid_image(&image);
    image.visible_size = INFS_BLOCK_SIZE * 2u;
    expect(open_image(&image) != 0, "reject truncated storage geometry");

    build_valid_image(&image);
    image.fail_reads = 1;
    expect(open_image(&image) != 0, "propagate storage read failure");

    free(image.bytes);
    puts("volume-conformance: PASS");
    return 0;
}
