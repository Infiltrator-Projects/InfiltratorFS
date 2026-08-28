// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/checksum.h"
#include "infilfs/endian.h"
#include "infilfs/format.h"
#include "infilfs/fs.h"
#include "infilfs/storage.h"
#include "infilfs/volume.h"
#include "test-allocation-tree.h"

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
    uint64_t fail_flush_after_block;
    int fail_flushes;
    int flush_failure_pending;
    uint64_t flush_calls;
    uint64_t fail_flush_call;
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
    if (image->fail_flushes && size == INFS_BLOCK_SIZE &&
        offset == image->fail_flush_after_block * INFS_BLOCK_SIZE)
        image->flush_failure_pending = 1;
    return INFS_STATUS_OK;
}

static infs_status memory_flush(void *context)
{
    struct memory_image *image = context;
    ++image->flush_calls;
    if (image->fail_flush_call &&
        image->flush_calls == image->fail_flush_call)
        return INFS_STATUS_IO_ERROR;
    if (image->flush_failure_pending && image->fail_flushes) {
        image->flush_failure_pending = 0;
        --image->fail_flushes;
        return INFS_STATUS_IO_ERROR;
    }
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
    image->fail_flush_after_block = UINT64_MAX;
    image->fail_flushes = 0;
    image->flush_failure_pending = 0;
    image->flush_calls = 0;
    image->fail_flush_call = 0;

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
    sb.free_blocks = infs_cpu_to_le64(TEST_BLOCKS - 9u);
    sb.allocation_root_block = infs_cpu_to_le64(TEST_ALLOCATION_ROOT_BLOCK);
    sb.allocation_leaf_count = infs_cpu_to_le64(1);
    sb.object_index_block = infs_cpu_to_le64(2);
    sb.root_object_block = infs_cpu_to_le64(3);
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        sb.checkpoint_block[i] = infs_cpu_to_le64(checkpoints[i]);
    memcpy(sb.filesystem_uuid, filesystem_id, sizeof(filesystem_id));
    memcpy(sb.root_object_id, root_id, sizeof(root_id));
    sb.incompat_flags = infs_cpu_to_le64(
        INFS_INCOMPAT_UTF8_NAMES | INFS_INCOMPAT_SPARSE_EXTENTS |
        INFS_INCOMPAT_ALLOCATION_TREE);
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

static void current_superblock(struct memory_image *image,
                               struct infs_superblock_disk *sb)
{
    expect(infs_decode_superblock(image->bytes, sb) == INFS_STATUS_OK,
           "decode current checkpoint");
}

static uint8_t *current_root_block(struct memory_image *image)
{
    struct infs_superblock_disk sb;
    current_superblock(image, &sb);
    return image->bytes +
        infs_le64_to_cpu(sb.root_object_block) * INFS_BLOCK_SIZE;
}

static struct infs_dirent_disk *raw_dir_find(uint8_t *directory,
                                             const char *name)
{
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)directory;
    struct infs_directory_payload_disk *payload =
        (struct infs_directory_payload_disk *)(directory + sizeof(*header));
    uint8_t *cursor = (uint8_t *)(payload + 1);
    size_t remaining = infs_le32_to_cpu(payload->bytes_used);
    size_t wanted = strlen(name);
    while (remaining) {
        struct infs_dirent_disk *entry =
            (struct infs_dirent_disk *)cursor;
        uint16_t rec = infs_le16_to_cpu(entry->record_size);
        uint16_t length = infs_le16_to_cpu(entry->name_length);
        if (length == wanted &&
            memcmp(cursor + sizeof(*entry), name, wanted) == 0)
            return entry;
        cursor += rec;
        remaining -= rec;
    }
    return NULL;
}

static uint64_t raw_index_find_block(struct memory_image *image,
                                     const uint8_t id[16], uint16_t *type_out)
{
    struct infs_superblock_disk sb;
    current_superblock(image, &sb);
    uint8_t *block = image->bytes +
        infs_le64_to_cpu(sb.object_index_block) * INFS_BLOCK_SIZE;
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)block;
    struct infs_index_payload_disk *payload =
        (struct infs_index_payload_disk *)(block + sizeof(*header));
    struct infs_index_entry_disk *entries =
        (struct infs_index_entry_disk *)(payload + 1);
    uint32_t count = infs_le32_to_cpu(payload->entry_count);
    for (uint32_t i = 0; i < count; ++i) {
        if (memcmp(entries[i].object_id, id, 16) == 0) {
            if (type_out)
                *type_out = infs_le16_to_cpu(entries[i].object_type);
            return infs_le64_to_cpu(entries[i].object_block);
        }
    }
    return UINT64_MAX;
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

static void check_canonical_padding(struct memory_image *image)
{
    build_valid_image(image);
    const uint64_t checkpoints[INFS_CHECKPOINT_COUNT] = {0, 2048, 4095};
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        uint8_t *block = image->bytes + checkpoints[i] * INFS_BLOCK_SIZE;
        struct infs_superblock_disk *sb = (struct infs_superblock_disk *)block;
        sb->checksum[8] = 1;
    }
    struct infs_volume volume;
    infs_status status = open_image(image, 0, &volume);
    expect(status != INFS_STATUS_OK, "reject nonzero checkpoint checksum reserve");
    close_if_open(&volume, status);

    build_valid_image(image);
    uint8_t *root = image->bytes + 3u * INFS_BLOCK_SIZE;
    root[INFS_BLOCK_SIZE - 1u] = 1;
    checksum_block(root, offsetof(struct infs_object_header_disk, checksum),
                   sizeof(((struct infs_object_header_disk *)root)->checksum));
    status = open_image(image, 0, &volume);
    expect(status != INFS_STATUS_OK, "reject nonzero metadata tail padding");
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

static void check_reserved_fields(struct memory_image *image)
{
    struct infs_volume volume;

    build_valid_image(image);
    uint8_t *root = image->bytes + 3u * INFS_BLOCK_SIZE;
    struct infs_directory_payload_disk *directory =
        (struct infs_directory_payload_disk *)(
            root + sizeof(struct infs_object_header_disk));
    directory->posix.flags = infs_cpu_to_le32(1);
    checksum_block(root, offsetof(struct infs_object_header_disk, checksum),
                   sizeof(((struct infs_object_header_disk *)root)->checksum));
    infs_status status = open_image(image, 0, &volume);
    expect(status != INFS_STATUS_OK, "reject nonzero POSIX reserved flags");
    close_if_open(&volume, status);

    build_valid_image(image);
    uint8_t *index = image->bytes + 2u * INFS_BLOCK_SIZE;
    struct infs_index_payload_disk *payload =
        (struct infs_index_payload_disk *)(
            index + sizeof(struct infs_object_header_disk));
    struct infs_index_entry_disk *entry =
        (struct infs_index_entry_disk *)(payload + 1);
    entry->flags = infs_cpu_to_le16(1);
    checksum_block(index, offsetof(struct infs_object_header_disk, checksum),
                   sizeof(((struct infs_object_header_disk *)index)->checksum));
    status = open_image(image, 0, &volume);
    expect(status != INFS_STATUS_OK, "reject nonzero index entry flags");
    close_if_open(&volume, status);
}

static void check_ro_compat_semantics(struct memory_image *image)
{
    build_valid_image(image);
    struct infs_superblock_disk sb;
    current_superblock(image, &sb);
    sb.ro_compat_flags = infs_cpu_to_le64(UINT64_C(0x8000000000000000));
    rewrite_all_checkpoints(image, &sb);

    struct infs_volume volume;
    infs_status status = open_image(image, 0, &volume);
    expect(status == INFS_STATUS_OK,
           "unknown ro-compatible feature opens read-only");
    infs_volume_close(&volume);
    status = open_image(image, 1, &volume);
    expect(status == INFS_STATUS_NOT_SUPPORTED,
           "unknown ro-compatible feature refuses writable open");
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

static void check_namespace_validation(struct memory_image *image)
{
    struct infs_volume volume;
    struct infs_lookup lookup;

    build_valid_image(image);
    expect(open_image(image, 1, &volume) == INFS_STATUS_OK,
           "open duplicate-name image");
    expect(infs_create_file(&volume, "/a", &file_options) == INFS_STATUS_OK,
           "create namespace a");
    expect(infs_create_file(&volume, "/b", &file_options) == INFS_STATUS_OK,
           "create namespace b");
    infs_volume_close(&volume);
    uint8_t *root = current_root_block(image);
    struct infs_dirent_disk *b = raw_dir_find(root, "b");
    expect(b != NULL, "find b directory entry");
    uint8_t *b_name = (uint8_t *)b + sizeof(*b);
    b_name[0] = 'a';
    checksum_block(root, offsetof(struct infs_object_header_disk, checksum),
                   sizeof(((struct infs_object_header_disk *)root)->checksum));
    infs_status status = open_image(image, 0, &volume);
    expect(status == INFS_STATUS_CORRUPT, "reject duplicate directory names");

    build_valid_image(image);
    expect(open_image(image, 1, &volume) == INFS_STATUS_OK,
           "open dangling-target image");
    expect(infs_create_file(&volume, "/a", &file_options) == INFS_STATUS_OK,
           "create dangling target");
    infs_volume_close(&volume);
    root = current_root_block(image);
    struct infs_dirent_disk *a = raw_dir_find(root, "a");
    expect(a != NULL, "find a directory entry");
    memset(a->object_id, 0xe5, sizeof(a->object_id));
    checksum_block(root, offsetof(struct infs_object_header_disk, checksum),
                   sizeof(((struct infs_object_header_disk *)root)->checksum));
    status = open_image(image, 0, &volume);
    expect(status == INFS_STATUS_CORRUPT, "reject dangling directory target");

    build_valid_image(image);
    expect(open_image(image, 1, &volume) == INFS_STATUS_OK,
           "open parent-mismatch image");
    expect(infs_create_file(&volume, "/a", &file_options) == INFS_STATUS_OK,
           "create parent-mismatch target");
    expect(infs_lookup_path(&volume, "/a", &lookup) == INFS_STATUS_OK,
           "lookup parent-mismatch target");
    uint64_t child_block = lookup.block;
    infs_volume_close(&volume);
    uint8_t *child = image->bytes + child_block * INFS_BLOCK_SIZE;
    struct infs_object_header_disk *child_header =
        (struct infs_object_header_disk *)child;
    memset(child_header->parent_id, 0, sizeof(child_header->parent_id));
    checksum_block(child, offsetof(struct infs_object_header_disk, checksum),
                   sizeof(child_header->checksum));
    status = open_image(image, 0, &volume);
    expect(status == INFS_STATUS_CORRUPT, "reject mismatched parent identity");

    build_valid_image(image);
    expect(open_image(image, 1, &volume) == INFS_STATUS_OK,
           "open stored-dot image");
    expect(infs_create_file(&volume, "/a", &file_options) == INFS_STATUS_OK,
           "create stored-dot target");
    infs_volume_close(&volume);
    root = current_root_block(image);
    a = raw_dir_find(root, "a");
    expect(a != NULL, "find stored-dot entry");
    uint8_t *a_name = (uint8_t *)a + sizeof(*a);
    a_name[0] = '.';
    checksum_block(root, offsetof(struct infs_object_header_disk, checksum),
                   sizeof(((struct infs_object_header_disk *)root)->checksum));
    status = open_image(image, 0, &volume);
    expect(status == INFS_STATUS_CORRUPT, "reject stored dot navigation entry");
}

static void check_checksum_reachability(struct memory_image *image)
{
    build_valid_image(image);
    struct infs_volume volume;
    expect(open_image(image, 1, &volume) == INFS_STATUS_OK,
           "open checksum reachability image");
    expect(infs_create_file(&volume, "/data", &file_options) == INFS_STATUS_OK,
           "create checksum reachability file");

    uint8_t data[INFS_BLOCK_SIZE];
    memset(data, 0x5a, sizeof(data));
    expect(infs_write_file(&volume, "/data", data, sizeof(data), 0) ==
               (int64_t)sizeof(data),
           "write low checksum segment");
    uint64_t high_logical = INFS_CHECKSUMS_PER_OBJECT + 8u;
    uint64_t high_offset = high_logical * INFS_BLOCK_SIZE;
    expect(infs_write_file(&volume, "/data", data, sizeof(data), high_offset) ==
               (int64_t)sizeof(data),
           "write high checksum segment");
    expect(infs_punch_hole(&volume, "/data", high_offset, INFS_BLOCK_SIZE) ==
               INFS_STATUS_OK,
           "punch high checksum data while retaining low data");

    struct infs_lookup file_lookup;
    expect(infs_lookup_path(&volume, "/data", &file_lookup) == INFS_STATUS_OK,
           "lookup checksum reachability file");
    infs_volume_close(&volume);

    uint8_t *file_block = image->bytes + file_lookup.block * INFS_BLOCK_SIZE;
    struct infs_file_payload_disk *file =
        (struct infs_file_payload_disk *)(
            file_block + sizeof(struct infs_object_header_disk));
    expect(memcmp(file->checksum_head_id, (uint8_t[16]){0}, 16) != 0,
           "checksum chain head exists");
    uint16_t type = 0;
    uint64_t first_block_no =
        raw_index_find_block(image, file->checksum_head_id, &type);
    expect(first_block_no != UINT64_MAX && type == INFS_OBJECT_CHECKSUM,
           "find checksum head object");
    uint8_t *first_block = image->bytes + first_block_no * INFS_BLOCK_SIZE;
    struct infs_checksum_payload_disk *first =
        (struct infs_checksum_payload_disk *)(
            first_block + sizeof(struct infs_object_header_disk));
    expect(memcmp(first->next_object_id, (uint8_t[16]){0}, 16) != 0,
           "inactive high checksum object retained");
    memset(first->next_object_id, 0, sizeof(first->next_object_id));
    expect(infs_object_finalize(first_block) == INFS_STATUS_OK,
           "finalize disconnected checksum head");

    infs_status status = open_image(image, 0, &volume);
    expect(status == INFS_STATUS_CORRUPT,
           "reject indexed checksum object unreachable from owner chain");
}

static void check_rename_replacement(struct memory_image *image)
{
    build_valid_image(image);
    struct infs_volume volume;
    expect(open_image(image, 1, &volume) == INFS_STATUS_OK,
           "open rename replacement image");

    expect(infs_create_file(&volume, "/src", &file_options) == INFS_STATUS_OK,
           "create rename source file");
    expect(infs_create_file(&volume, "/dst", &file_options) == INFS_STATUS_OK,
           "create rename destination file");
    expect(infs_write_file(&volume, "/src", "new", 3, 0) == 3,
           "write rename source file");
    expect(infs_write_file(&volume, "/dst", "old", 3, 0) == 3,
           "write rename destination file");
    struct infs_attributes source_attributes;
    expect(infs_get_attributes(&volume, "/src", &source_attributes) ==
               INFS_STATUS_OK,
           "get source identity before replacement");
    expect(infs_rename(&volume, "/src", "/dst") == INFS_STATUS_OK,
           "replace existing file with rename");
    expect(infs_get_attributes(&volume, "/src", &source_attributes) ==
               INFS_STATUS_NOT_FOUND,
           "rename replacement removes old source name");
    struct infs_attributes destination_attributes;
    expect(infs_get_attributes(&volume, "/dst", &destination_attributes) ==
               INFS_STATUS_OK,
           "lookup replaced destination");
    char content[4] = {0};
    expect(infs_read_file(&volume, "/dst", content, 3, 0) == 3 &&
               memcmp(content, "new", 3) == 0,
           "replacement destination contains source data");
    expect(infs_rename(&volume, "/dst", "/dst") == INFS_STATUS_OK,
           "rename object onto itself is a no-op");

    expect(infs_mkdir(&volume, "/from", &dir_options) == INFS_STATUS_OK,
           "create same-parent source directory");
    expect(infs_mkdir(&volume, "/to", &dir_options) == INFS_STATUS_OK,
           "create same-parent destination directory");
    expect(infs_rename(&volume, "/from", "/to") == INFS_STATUS_OK,
           "replace empty directory with rename");

    expect(infs_mkdir(&volume, "/left", &dir_options) == INFS_STATUS_OK,
           "create nonempty replacement source");
    expect(infs_mkdir(&volume, "/right", &dir_options) == INFS_STATUS_OK,
           "create nonempty replacement destination");
    expect(infs_create_file(&volume, "/right/file", &file_options) ==
               INFS_STATUS_OK,
           "populate replacement destination directory");
    expect(infs_rename(&volume, "/left", "/right") == INFS_STATUS_NOT_EMPTY,
           "refuse replacement of nonempty directory");
    expect(infs_get_attributes(&volume, "/left", &destination_attributes) ==
               INFS_STATUS_OK,
           "failed replacement preserves source directory");
    expect(infs_get_attributes(&volume, "/right/file", &destination_attributes) ==
               INFS_STATUS_OK,
           "failed replacement preserves destination contents");

    expect(infs_rename(&volume, "/dst", "/right") == INFS_STATUS_IS_DIRECTORY,
           "file cannot replace directory");
    expect(infs_rename(&volume, "/left", "/dst") == INFS_STATUS_NOT_DIRECTORY,
           "directory cannot replace file");

    expect(infs_mkdir(&volume, "/p", &dir_options) == INFS_STATUS_OK,
           "create cross-parent p");
    expect(infs_mkdir(&volume, "/q", &dir_options) == INFS_STATUS_OK,
           "create cross-parent q");
    expect(infs_mkdir(&volume, "/p/move", &dir_options) == INFS_STATUS_OK,
           "create cross-parent move directory");
    expect(infs_mkdir(&volume, "/q/replace", &dir_options) == INFS_STATUS_OK,
           "create cross-parent replacement directory");
    expect(infs_rename(&volume, "/p/move", "/q/replace") == INFS_STATUS_OK,
           "cross-parent directory replacement");

    infs_volume_close(&volume);
    expect(open_image(image, 0, &volume) == INFS_STATUS_OK,
           "reopen after rename replacement matrix");
    expect(infs_get_attributes(&volume, "/q/replace", &destination_attributes) ==
               INFS_STATUS_OK,
           "cross-parent replacement persists");
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
    expect(infs_create_file(&volume, "/committed", &file_options) ==
               INFS_STATUS_OK,
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

static void check_precheckpoint_flush_failure_aborts(
    struct memory_image *image)
{
    build_valid_image(image);
    struct infs_volume volume;
    struct infs_lookup lookup;
    expect(open_image(image, 1, &volume) == INFS_STATUS_OK,
           "open writable pre-checkpoint-flush image");
    expect(infs_volume_set_deferred_publish(
               &volume, 1, UINT64_MAX) == INFS_STATUS_OK,
           "enable deferred publish for pre-checkpoint failure");

    uint64_t base_generation = infs_le64_to_cpu(volume.sb.generation);
    uint64_t base_free = infs_le64_to_cpu(volume.sb.free_blocks);
    expect(infs_create_file(&volume, "/staged", &file_options) ==
               INFS_STATUS_OK && volume.tx_active,
           "stage transaction before forced flush failure");

    /*
     * infs_volume_sync() performs one durability flush before publishing the
     * allocation tree and one after writing its replacement pages. Fail the
     * latter: no checkpoint has yet been attempted, so rollback is certain.
     */
    image->fail_flush_call = image->flush_calls + 2u;
    expect(infs_volume_sync(&volume) == INFS_STATUS_IO_ERROR,
           "surface post-allocation-map pre-checkpoint flush failure");
    expect(!volume.tx_active && volume.tx_error == INFS_STATUS_OK,
           "abort failed pre-checkpoint transaction");
    expect(infs_le64_to_cpu(volume.sb.generation) == base_generation,
           "restore committed generation after pre-checkpoint failure");
    expect(infs_le64_to_cpu(volume.sb.free_blocks) == base_free,
           "restore committed free-block accounting after pre-checkpoint failure");
    expect(infs_lookup_path(&volume, "/staged", &lookup) ==
               INFS_STATUS_NOT_FOUND,
           "discard staged namespace after pre-checkpoint failure");

    image->fail_flush_call = 0;
    expect(infs_create_file(&volume, "/after-failure", &file_options) ==
               INFS_STATUS_OK,
           "permit clean transaction after pre-checkpoint rollback");
    expect(infs_volume_sync(&volume) == INFS_STATUS_OK,
           "commit clean transaction after pre-checkpoint rollback");
    infs_volume_close(&volume);

    expect(open_image(image, 0, &volume) == INFS_STATUS_OK,
           "reopen image after pre-checkpoint rollback");
    expect(infs_lookup_path(&volume, "/staged", &lookup) ==
               INFS_STATUS_NOT_FOUND,
           "staged file absent after reopen");
    expect(infs_lookup_path(&volume, "/after-failure", &lookup) ==
               INFS_STATUS_OK,
           "later committed file survives reopen");
    infs_volume_close(&volume);
}

static void check_indeterminate_commit_requires_reopen(
    struct memory_image *image)
{
    build_valid_image(image);
    struct infs_volume volume;
    expect(open_image(image, 1, &volume) == INFS_STATUS_OK,
           "open writable indeterminate-commit image");

    /* Generation two publishes at checkpoint index two (block 4095). The
     * write reaches the backend, but the following flush reports failure, so
     * the caller cannot know whether generation one or two is durable. */
    image->fail_flush_after_block = TEST_BLOCKS - 1u;
    image->fail_flushes = 1;
    expect(infs_create_file(&volume, "/uncertain", &file_options) ==
               INFS_STATUS_IO_ERROR,
           "surface commit-checkpoint flush failure");
    expect(volume.reopen_required_status == INFS_STATUS_IO_ERROR,
           "record that checkpoint recovery is required");
    expect(!volume.writable,
           "disable mutation after an indeterminate commit");
    expect(infs_create_file(&volume, "/must-not-write", &file_options) ==
               INFS_STATUS_IO_ERROR,
           "reject later mutation until reopen");
    expect(infs_volume_sync(&volume) == INFS_STATUS_IO_ERROR,
           "sync continues to report the recovery requirement");
    infs_volume_close(&volume);

    image->fail_flush_after_block = UINT64_MAX;
    image->fail_flushes = 0;
    expect(open_image(image, 0, &volume) == INFS_STATUS_OK,
           "recover indeterminate commit by reopening checkpoints");
    struct infs_lookup lookup;
    expect(infs_lookup_path(&volume, "/uncertain", &lookup) == INFS_STATUS_OK,
           "reopen selects the checkpoint that reached storage");
    expect(infs_lookup_path(&volume, "/must-not-write", &lookup) ==
               INFS_STATUS_NOT_FOUND,
           "poisoned volume performed no later mutation");
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
    check_canonical_padding(&image);
    check_object_version(&image);
    check_reserved_fields(&image);
    check_ro_compat_semantics(&image);
    check_bitmap_ownership(&image);
    check_read_status(&image);
    check_namespace_validation(&image);
    check_checksum_reachability(&image);
    check_rename_replacement(&image);
    check_post_commit_replica_failure(&image);
    check_precheckpoint_flush_failure_aborts(&image);
    check_indeterminate_commit_requires_reopen(&image);

    free(image.bytes);
    puts("hardening-conformance: PASS");
    return 0;
}
