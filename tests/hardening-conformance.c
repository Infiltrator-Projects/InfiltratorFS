// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/checksum.h"
#include "infilfs/endian.h"
#include "infilfs/format.h"
#include "infilfs/fs.h"
#include "infilfs/storage.h"
#include "infilfs/volume.h"

#include <stddef.h>
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
    uint64_t fail_write_block;
    int fail_writes;
};

static void fail(const char *message)
{
    fprintf(stderr, "hardening-conformance: %s\n", message);
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
    if (image->fail_writes && size == INFS_BLOCK_SIZE &&
        offset == image->fail_write_block * INFS_BLOCK_SIZE)
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

static void bitmap_set(uint8_t *bitmap, uint64_t block, int allocated)
{
    uint8_t mask = (uint8_t)(1u << (block & 7u));
    if (allocated)
        bitmap[block >> 3] |= mask;
    else
        bitmap[block >> 3] &= (uint8_t)~mask;
}

static void checksum_block(uint8_t block[INFS_BLOCK_SIZE], size_t checksum_offset,
                           size_t checksum_size)
{
    memset(block + checksum_offset, 0, checksum_size);
    uint64_t crc = infs_cpu_to_le64(infs_crc64_ecma(block, INFS_BLOCK_SIZE));
    memcpy(block + checksum_offset, &crc, sizeof(crc));
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

    memset(image->bytes, 0, image->size);
    image->random_state = UINT64_C(0x9e3779b97f4a7c15);
    image->fail_write_block = UINT64_MAX;
    image->fail_writes = 0;

    uint8_t *bitmap = image->bytes + INFS_BLOCK_SIZE;
    bitmap_set(bitmap, 0, 1);
    bitmap_set(bitmap, 1, 1);
    bitmap_set(bitmap, 2, 1);
    bitmap_set(bitmap, 3, 1);
    bitmap_set(bitmap, checkpoints[1], 1);
    bitmap_set(bitmap, checkpoints[2], 1);
    for (uint64_t block = TEST_BLOCKS; block < INFS_BLOCK_SIZE * 8u; ++block)
        bitmap_set(bitmap, block, 1);

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
    memcpy(entry->object_id, root_id, sizeof(entry->object_id));
    entry->object_block = infs_cpu_to_le64(3);
    entry->object_type = infs_cpu_to_le16(INFS_OBJECT_DIRECTORY);
    index_header->payload_size = infs_cpu_to_le32(
        sizeof(*index_payload) + sizeof(*entry));
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
    sb.free_blocks = infs_cpu_to_le64(TEST_BLOCKS - 6u);
    sb.bitmap_start_block = infs_cpu_to_le64(1);
    sb.bitmap_block_count = infs_cpu_to_le64(1);
    sb.object_index_block = infs_cpu_to_le64(2);
    sb.root_object_block = infs_cpu_to_le64(3);
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        sb.checkpoint_block[i] = infs_cpu_to_le64(checkpoints[i]);
    memcpy(sb.filesystem_uuid, filesystem_id, sizeof(filesystem_id));
    memcpy(sb.root_object_id, root_id, sizeof(root_id));
    sb.incompat_flags = infs_cpu_to_le64(
        INFS_INCOMPAT_UTF8_NAMES | INFS_INCOMPAT_SPARSE_EXTENTS);
    memcpy(sb.label, "Hardening", 9);
    expect(infs_encode_superblock(block, &sb) == INFS_STATUS_OK,
           "encode checkpoint");
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        memcpy(image->bytes + checkpoints[i] * INFS_BLOCK_SIZE,
               block, sizeof(block));
}

static infs_status open_image(struct memory_image *image, int writable,
                              struct infs_volume *volume)
{
    struct infs_storage storage = make_storage(image);
    return infs_volume_open_storage(volume, &storage, writable);
}

static void close_if_open(struct infs_volume *volume, infs_status status)
{
    if (status == INFS_STATUS_OK)
        infs_volume_close(volume);
}

static void rewrite_all_checkpoints(struct memory_image *image,
                                    const struct infs_superblock_disk *sb)
{
    const uint64_t checkpoints[INFS_CHECKPOINT_COUNT] = {0, 2048, 4095};
    uint8_t block[INFS_BLOCK_SIZE];
    expect(infs_encode_superblock(block, sb) == INFS_STATUS_OK,
           "rewrite checkpoints");
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        memcpy(image->bytes + checkpoints[i] * INFS_BLOCK_SIZE,
               block, sizeof(block));
}

static void check_label_validation(struct memory_image *image)
{
    build_valid_image(image);
    const uint64_t checkpoints[INFS_CHECKPOINT_COUNT] = {0, 2048, 4095};
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        uint8_t *block = image->bytes + checkpoints[i] * INFS_BLOCK_SIZE;
        struct infs_superblock_disk *sb = (struct infs_superblock_disk *)block;
        memset(sb->label, 'A', sizeof(sb->label));
        checksum_block(block, offsetof(struct infs_superblock_disk, checksum),
                       sizeof(sb->checksum));
    }
    struct infs_volume volume;
    infs_status status = open_image(image, 0, &volume);
    expect(status != INFS_STATUS_OK, "reject non-terminated checkpoint label");
    close_if_open(&volume, status);
}

static void check_object_version(struct memory_image *image)
{
    build_valid_image(image);
    uint8_t *root = image->bytes + 3u * INFS_BLOCK_SIZE;
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)root;
    header->object_version = infs_cpu_to_le16(2);
    checksum_block(root, offsetof(struct infs_object_header_disk, checksum),
                   sizeof(header->checksum));

    struct infs_volume volume;
    infs_status status = open_image(image, 0, &volume);
    expect(status != INFS_STATUS_OK, "reject unsupported object version");
    close_if_open(&volume, status);
}

static void check_bitmap_ownership(struct memory_image *image)
{
    build_valid_image(image);
    bitmap_set(image->bytes + INFS_BLOCK_SIZE, 4, 1);

    struct infs_superblock_disk sb;
    memcpy(&sb, image->bytes, sizeof(sb));
    sb.free_blocks = infs_cpu_to_le64(TEST_BLOCKS - 7u);
    rewrite_all_checkpoints(image, &sb);

    struct infs_volume volume;
    infs_status status = open_image(image, 0, &volume);
    expect(status != INFS_STATUS_OK,
           "reject allocated block with no live owner");
    close_if_open(&volume, status);
}

static void check_read_status(struct memory_image *image)
{
    build_valid_image(image);
    struct infs_volume volume;
    expect(open_image(image, 0, &volume) == INFS_STATUS_OK,
           "open read-status image");
    uint8_t byte = 0;
    expect(infs_read_file(&volume, "/missing", &byte, 1, 0) ==
               INFS_STATUS_NOT_FOUND,
           "preserve NOT_FOUND from read path");
    infs_volume_close(&volume);
}

static void check_post_commit_replica_failure(struct memory_image *image)
{
    build_valid_image(image);
    struct infs_volume volume;
    expect(open_image(image, 1, &volume) == INFS_STATUS_OK,
           "open writable replica test image");

    image->fail_write_block = 0;
    image->fail_writes = 1;
    const struct infs_create_options options = {
        .posix_permissions = 0644,
        .posix_uid = 1000,
        .posix_gid = 1000,
    };
    expect(infs_create_file(&volume, "/committed", &options) == INFS_STATUS_OK,
           "committed mutation survives replica failure");
    expect(volume.checkpoint_repair_needed != 0,
           "record degraded checkpoint replication");

    struct infs_lookup lookup;
    expect(infs_lookup_path(&volume, "/committed", &lookup) == INFS_STATUS_OK,
           "committed namespace change remains visible");

    image->fail_writes = 0;
    expect(infs_volume_sync(&volume) == INFS_STATUS_OK,
           "heal checkpoint replicas on sync");
    expect(volume.checkpoint_repair_needed == 0,
           "clear checkpoint repair state after heal");
    infs_volume_close(&volume);

    expect(open_image(image, 0, &volume) == INFS_STATUS_OK,
           "reopen healed image");
    expect(infs_lookup_path(&volume, "/committed", &lookup) == INFS_STATUS_OK,
           "committed file survives reopen");
    infs_volume_close(&volume);
}

int main(void)
{
    struct memory_image image = {
        .bytes = malloc(TEST_SIZE),
        .size = TEST_SIZE,
    };
    if (!image.bytes)
        fail("allocate memory image");

    struct infs_volume volume;
    build_valid_image(&image);
    expect(open_image(&image, 0, &volume) == INFS_STATUS_OK,
           "accept deterministic valid image");
    infs_volume_close(&volume);

    check_label_validation(&image);
    check_object_version(&image);
    check_bitmap_ownership(&image);
    check_read_status(&image);
    check_post_commit_replica_failure(&image);

    free(image.bytes);
    puts("hardening-conformance: PASS");
    return 0;
}
