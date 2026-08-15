// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/endian.h"
#include "infilfs/format.h"
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
    size_t visible_size;
    int fail_reads;
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

static const struct infs_storage_ops memory_ops = {
    .read_at = memory_read,
    .get_size = memory_size,
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
    sb.free_blocks = infs_cpu_to_le64(TEST_BLOCKS - 6u);
    sb.bitmap_start_block = infs_cpu_to_le64(1);
    sb.bitmap_block_count = infs_cpu_to_le64(1);
    sb.object_index_block = infs_cpu_to_le64(2);
    sb.root_object_block = infs_cpu_to_le64(3);
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        sb.checkpoint_block[i] = infs_cpu_to_le64(checkpoints[i]);
    memcpy(sb.filesystem_uuid, filesystem_id, 16);
    memcpy(sb.root_object_id, root_id, 16);
    sb.incompat_flags = infs_cpu_to_le64(INFS_INCOMPAT_UTF8_NAMES);
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
        INFS_INCOMPAT_UTF8_NAMES | (UINT64_C(1) << 63));
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
