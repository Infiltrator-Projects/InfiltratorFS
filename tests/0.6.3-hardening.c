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
    size_t size;
    uint64_t random_state;
    uint64_t fail_read_block;
    int fail_reads;
    int fail_checkpoint_reads;
};

static void fail(const char *message)
{
    fprintf(stderr, "0.6.3-hardening: %s\n", message);
    exit(1);
}

static void expect(int condition, const char *message)
{
    if (!condition)
        fail(message);
}

static int is_checkpoint_block(uint64_t block)
{
    return block == 0 || block == TEST_BLOCKS / 2u ||
           block == TEST_BLOCKS - 1u;
}

static infs_status memory_read(void *context, uint64_t offset,
                               void *buffer, size_t size)
{
    struct memory_image *image = context;
    if (offset > image->size || size > image->size - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
    if (size == INFS_BLOCK_SIZE) {
        uint64_t block = offset / INFS_BLOCK_SIZE;
        if ((image->fail_reads && block == image->fail_read_block) ||
            (image->fail_checkpoint_reads && is_checkpoint_block(block)))
            return INFS_STATUS_IO_ERROR;
    }
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

static infs_status open_image(struct memory_image *image, int writable,
                              struct infs_volume *volume)
{
    struct infs_storage storage = make_storage(image);
    return infs_volume_open_storage(volume, &storage, writable);
}

static void bitmap_set(uint8_t *bitmap, uint64_t block)
{
    bitmap[block >> 3] |= (uint8_t)(1u << (block & 7u));
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
    image->fail_read_block = UINT64_MAX;
    image->fail_reads = 0;
    image->fail_checkpoint_reads = 0;

    uint8_t *bitmap = image->bytes + INFS_BLOCK_SIZE;
    bitmap_set(bitmap, 0);
    bitmap_set(bitmap, 1);
    bitmap_set(bitmap, 2);
    bitmap_set(bitmap, 3);
    bitmap_set(bitmap, checkpoints[1]);
    bitmap_set(bitmap, checkpoints[2]);
    for (uint64_t block = TEST_BLOCKS; block < INFS_BLOCK_SIZE * 8u; ++block)
        bitmap_set(bitmap, block);

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
    index_payload->reserved = 0;
    memset(entry, 0, sizeof(*entry));
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
    memcpy(sb.label, "0.6.3", 5);
    expect(infs_encode_superblock(block, &sb) == INFS_STATUS_OK,
           "encode checkpoint");
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        memcpy(image->bytes + checkpoints[i] * INFS_BLOCK_SIZE,
               block, sizeof(block));
}

static const struct infs_create_options file_options = {
    .posix_permissions = 0644,
    .posix_uid = 1000,
    .posix_gid = 1000,
};

static const struct infs_create_options dir_options = {
    .posix_permissions = 0755,
    .posix_uid = 1000,
    .posix_gid = 1000,
};

static uint64_t prepare_newer_corrupt_generation(struct memory_image *image,
                                                 uint8_t old_checkpoint[INFS_BLOCK_SIZE])
{
    const uint64_t newer_checkpoint_block = 2048;
    build_valid_image(image);
    memcpy(old_checkpoint, image->bytes, INFS_BLOCK_SIZE);

    struct infs_volume volume;
    expect(open_image(image, 1, &volume) == INFS_STATUS_OK,
           "open image for newer generation");
    expect(infs_create_file(&volume, "/newer", &file_options) == INFS_STATUS_OK,
           "commit newer generation");
    infs_volume_close(&volume);

    struct infs_superblock_disk newer;
    expect(infs_decode_superblock(
               image->bytes + newer_checkpoint_block * INFS_BLOCK_SIZE,
               &newer) == INFS_STATUS_OK,
           "decode newer checkpoint");
    expect(infs_le64_to_cpu(newer.generation) == 2u,
           "newer checkpoint generation is two");
    uint64_t newer_root = infs_le64_to_cpu(newer.root_object_block);
    expect(newer_root != 3u, "newer root is copy-on-write relocated");

    image->bytes[newer_root * INFS_BLOCK_SIZE + 257u] ^= 0x5au;
    memcpy(image->bytes, old_checkpoint, INFS_BLOCK_SIZE);
    return newer_root;
}

static void check_checkpoint_replica_reads(struct memory_image *image)
{
    build_valid_image(image);
    image->fail_read_block = 0;
    image->fail_reads = 1;
    struct infs_volume volume;
    expect(open_image(image, 0, &volume) == INFS_STATUS_OK,
           "one unreadable checkpoint replica is tolerated");
    infs_volume_close(&volume);

    image->fail_reads = 1;
    expect(open_image(image, 1, &volume) == INFS_STATUS_IO_ERROR,
           "writable open refuses an unreadable checkpoint replica");
    image->fail_reads = 0;

    build_valid_image(image);
    image->fail_checkpoint_reads = 1;
    expect(open_image(image, 0, &volume) == INFS_STATUS_IO_ERROR,
           "all unreadable checkpoint replicas preserve I/O failure");
    image->fail_checkpoint_reads = 0;
}

static void check_unreadable_newer_checkpoint_is_preserved(
    struct memory_image *image)
{
    const uint64_t checkpoints[INFS_CHECKPOINT_COUNT] = {0, 2048, 4095};
    uint8_t old_checkpoint[INFS_BLOCK_SIZE];
    build_valid_image(image);
    memcpy(old_checkpoint, image->bytes, sizeof(old_checkpoint));

    struct infs_volume volume;
    expect(open_image(image, 1, &volume) == INFS_STATUS_OK,
           "open image for mixed-generation preservation test");
    expect(infs_create_file(&volume, "/newer", &file_options) == INFS_STATUS_OK,
           "commit generation for unreadable-replica test");
    infs_volume_close(&volume);

    /* Model a crash after generation two's primary checkpoint (block 4095)
     * became durable but before its two replicas were updated. */
    memcpy(image->bytes + checkpoints[0] * INFS_BLOCK_SIZE,
           old_checkpoint, sizeof(old_checkpoint));
    memcpy(image->bytes + checkpoints[1] * INFS_BLOCK_SIZE,
           old_checkpoint, sizeof(old_checkpoint));
    uint8_t newer_checkpoint[INFS_BLOCK_SIZE];
    memcpy(newer_checkpoint,
           image->bytes + checkpoints[2] * INFS_BLOCK_SIZE,
           sizeof(newer_checkpoint));

    image->fail_read_block = checkpoints[2];
    image->fail_reads = 1;
    expect(open_image(image, 0, &volume) == INFS_STATUS_OK,
           "read-only open may inspect surviving older replicas");
    expect(infs_le64_to_cpu(volume.sb.generation) == 1u,
           "read-only inspection selected the visible older generation");
    infs_volume_close(&volume);

    image->fail_reads = 1;
    expect(open_image(image, 1, &volume) == INFS_STATUS_IO_ERROR,
           "writable open does not guess past an unreadable newer replica");
    image->fail_reads = 0;
    expect(memcmp(newer_checkpoint,
                  image->bytes + checkpoints[2] * INFS_BLOCK_SIZE,
                  sizeof(newer_checkpoint)) == 0,
           "failed writable open preserves the possible newer checkpoint");
}

static void check_checkpoint_graph_fallback(struct memory_image *image)
{
    uint8_t old_checkpoint[INFS_BLOCK_SIZE];
    (void)prepare_newer_corrupt_generation(image, old_checkpoint);

    struct infs_volume volume;
    expect(open_image(image, 0, &volume) == INFS_STATUS_OK,
           "fall back to older valid graph on read-only open");
    expect(infs_le64_to_cpu(volume.sb.generation) == 1u,
           "read-only fallback selected generation one");
    struct infs_lookup lookup;
    expect(infs_lookup_path(&volume, "/newer", &lookup) == INFS_STATUS_NOT_FOUND,
           "rolled-back namespace excludes corrupt newer generation");
    infs_volume_close(&volume);

    expect(open_image(image, 1, &volume) == INFS_STATUS_OK,
           "fall back to older valid graph on writable open");
    expect(infs_le64_to_cpu(volume.sb.generation) == 1u,
           "writable fallback selected generation one");
    infs_volume_close(&volume);

    const uint64_t checkpoints[INFS_CHECKPOINT_COUNT] = {0, 2048, 4095};
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        struct infs_superblock_disk healed;
        expect(infs_decode_superblock(
                   image->bytes + checkpoints[i] * INFS_BLOCK_SIZE,
                   &healed) == INFS_STATUS_OK,
               "decode healed checkpoint");
        expect(infs_le64_to_cpu(healed.generation) == 1u,
               "writable fallback healed replicas to selected generation");
    }
}

static void check_checkpoint_graph_io_is_not_masked(struct memory_image *image)
{
    uint8_t old_checkpoint[INFS_BLOCK_SIZE];
    uint64_t newer_root =
        prepare_newer_corrupt_generation(image, old_checkpoint);

    image->fail_read_block = newer_root;
    image->fail_reads = 1;
    struct infs_volume volume;
    expect(open_image(image, 0, &volume) == INFS_STATUS_IO_ERROR,
           "do not hide newer-generation graph I/O failure by falling back");
    image->fail_reads = 0;
}

static void check_rename_trailing_slashes(struct memory_image *image)
{
    build_valid_image(image);
    struct infs_volume volume;
    expect(open_image(image, 1, &volume) == INFS_STATUS_OK,
           "open rename slash image");
    expect(infs_create_file(&volume, "/file", &file_options) == INFS_STATUS_OK,
           "create rename slash file");
    expect(infs_mkdir(&volume, "/from", &dir_options) == INFS_STATUS_OK,
           "create rename slash source directory");
    expect(infs_mkdir(&volume, "/to", &dir_options) == INFS_STATUS_OK,
           "create rename slash destination directory");

    expect(infs_rename(&volume, "/file/", "/renamed") ==
               INFS_STATUS_NOT_DIRECTORY,
           "source trailing slash requires a directory");
    expect(infs_rename(&volume, "/file", "/missing/") ==
               INFS_STATUS_NOT_FOUND,
           "nonexistent destination with trailing slash is not stripped");
    expect(infs_rename(&volume, "/from/", "/to/") == INFS_STATUS_OK,
           "directory trailing slashes retain directory semantics");

    struct infs_attributes attributes;
    expect(infs_get_attributes(&volume, "/file", &attributes) == INFS_STATUS_OK,
           "failed file rename preserves source");
    expect(infs_get_attributes(&volume, "/from", &attributes) ==
               INFS_STATUS_NOT_FOUND,
           "directory source moved");
    expect(infs_get_attributes(&volume, "/to", &attributes) == INFS_STATUS_OK &&
               attributes.object_type == INFS_OBJECT_DIRECTORY,
           "directory destination persists");
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

    check_checkpoint_replica_reads(&image);
    check_unreadable_newer_checkpoint_is_preserved(&image);
    check_checkpoint_graph_fallback(&image);
    check_checkpoint_graph_io_is_not_masked(&image);
    check_rename_trailing_slashes(&image);

    free(image.bytes);
    puts("0.6.3-hardening: PASS");
    return 0;
}
