// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/fs.h"

#include "infilfs/checksum.h"
#include "infilfs/endian.h"
#include "infilfs/storage.h"
#include "infilfs/utf8.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static uint64_t block_crc_with_zeroed_checksum(const uint8_t block[INFS_BLOCK_SIZE],
                                                size_t checksum_offset,
                                                size_t checksum_size)
{
    uint8_t tmp[INFS_BLOCK_SIZE];
    memcpy(tmp, block, sizeof(tmp));
    memset(tmp + checksum_offset, 0, checksum_size);
    return infs_crc64_ecma(tmp, sizeof(tmp));
}

static int bytes_are_zero(const uint8_t *bytes, size_t size)
{
    uint8_t combined = 0;
    for (size_t i = 0; i < size; ++i)
        combined |= bytes[i];
    return combined == 0;
}

static int id_is_nonzero(const uint8_t id[16])
{
    return !bytes_are_zero(id, 16);
}

static int superblock_label_valid(const uint8_t label[INFS_LABEL_MAX])
{
    const uint8_t *end = memchr(label, 0, INFS_LABEL_MAX);
    if (!end)
        return 0;
    size_t length = (size_t)(end - label);
    if (length && !infs_utf8_validate(label, length))
        return 0;
    for (size_t i = length + 1u; i < INFS_LABEL_MAX; ++i) {
        if (label[i] != 0)
            return 0;
    }
    return 1;
}

static int object_type_valid(uint16_t type)
{
    return type == INFS_OBJECT_DIRECTORY || type == INFS_OBJECT_FILE ||
           type == INFS_OBJECT_INDEX || type == INFS_OBJECT_CHECKSUM;
}

infs_status infs_encode_superblock(uint8_t block[INFS_BLOCK_SIZE],
                                   const struct infs_superblock_disk *sb)
{
    if (!block || !sb || !superblock_label_valid(sb->label) ||
        !id_is_nonzero(sb->filesystem_uuid) ||
        !id_is_nonzero(sb->root_object_id))
        return INFS_STATUS_INVALID_ARGUMENT;
    memset(block, 0, INFS_BLOCK_SIZE);
    memcpy(block, sb, sizeof(*sb));

    const size_t off = offsetof(struct infs_superblock_disk, checksum);
    memset(block + off, 0, sizeof(sb->checksum));
    uint64_t crc = infs_cpu_to_le64(block_crc_with_zeroed_checksum(
        block, off, sizeof(sb->checksum)));
    memcpy(block + off, &crc, sizeof(crc));
    return INFS_STATUS_OK;
}

int infs_validate_superblock_block(const uint8_t block[INFS_BLOCK_SIZE])
{
    const struct infs_superblock_disk *sb =
        (const struct infs_superblock_disk *)block;

    if (memcmp(sb->magic, INFS_MAGIC, 8) != 0)
        return 0;
    if (infs_le16_to_cpu(sb->format_major) != INFS_FORMAT_MAJOR)
        return 0;
    uint16_t format_minor = infs_le16_to_cpu(sb->format_minor);
    if (format_minor > INFS_FORMAT_MINOR)
        return 0;
    if (infs_le16_to_cpu(sb->header_size) != sizeof(*sb))
        return 0;
    if (infs_le16_to_cpu(sb->block_shift) != INFS_BLOCK_SHIFT)
        return 0;
    if (infs_le32_to_cpu(sb->checksum_type) != INFS_CHECKSUM_CRC64_ECMA)
        return 0;
    if (infs_le64_to_cpu(sb->generation) == 0)
        return 0;
    if (!id_is_nonzero(sb->filesystem_uuid) ||
        !id_is_nonzero(sb->root_object_id))
        return 0;
    if (!superblock_label_valid(sb->label))
        return 0;
    uint64_t incompat_flags = infs_le64_to_cpu(sb->incompat_flags);
    uint64_t required_flags = INFS_INCOMPAT_UTF8_NAMES;
    if (format_minor >= 6u)
        required_flags |= INFS_INCOMPAT_SPARSE_EXTENTS;
    if ((incompat_flags & ~INFS_KNOWN_INCOMPAT_FLAGS) != 0 ||
        (incompat_flags & required_flags) != required_flags ||
        (format_minor < 6u &&
         (incompat_flags & INFS_INCOMPAT_SPARSE_EXTENTS) != 0))
        return 0;

    const size_t off = offsetof(struct infs_superblock_disk, checksum);
    if (!bytes_are_zero(sb->checksum + sizeof(uint64_t),
                        sizeof(sb->checksum) - sizeof(uint64_t)) ||
        !bytes_are_zero(block + sizeof(*sb), INFS_BLOCK_SIZE - sizeof(*sb)))
        return 0;

    uint64_t stored_le;
    memcpy(&stored_le, block + off, sizeof(stored_le));
    uint64_t stored = infs_le64_to_cpu(stored_le);
    uint64_t actual = block_crc_with_zeroed_checksum(
        block, off, sizeof(sb->checksum));
    return stored == actual;
}

infs_status infs_decode_superblock(const uint8_t block[INFS_BLOCK_SIZE],
                                   struct infs_superblock_disk *sb)
{
    if (!sb || !infs_validate_superblock_block(block))
        return INFS_STATUS_INVALID_ARGUMENT;
    memcpy(sb, block, sizeof(*sb));
    return INFS_STATUS_OK;
}

infs_status infs_object_init(uint8_t block[INFS_BLOCK_SIZE],
                             uint16_t object_type,
                             const uint8_t object_id[16],
                             const uint8_t parent_id[16],
                             uint64_t generation, uint32_t payload_size)
{
    if (!block || !object_id || !id_is_nonzero(object_id) ||
        !object_type_valid(object_type) || generation == 0 ||
        payload_size > INFS_BLOCK_SIZE - sizeof(struct infs_object_header_disk))
        return INFS_STATUS_INVALID_ARGUMENT;

    memset(block, 0, INFS_BLOCK_SIZE);
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)block;
    memcpy(hdr->magic, INFS_OBJECT_MAGIC, 8);
    hdr->object_type = infs_cpu_to_le16(object_type);
    hdr->object_version = infs_cpu_to_le16(1);
    hdr->header_size = infs_cpu_to_le32(sizeof(*hdr));
    hdr->generation = infs_cpu_to_le64(generation);
    memcpy(hdr->object_id, object_id, 16);
    if (parent_id)
        memcpy(hdr->parent_id, parent_id, 16);
    hdr->payload_size = infs_cpu_to_le32(payload_size);
    hdr->checksum_type = infs_cpu_to_le32(INFS_CHECKSUM_CRC64_ECMA);
    return INFS_STATUS_OK;
}

infs_status infs_object_finalize(uint8_t block[INFS_BLOCK_SIZE])
{
    if (!block)
        return INFS_STATUS_INVALID_ARGUMENT;
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)block;
    uint16_t object_type = infs_le16_to_cpu(hdr->object_type);
    uint32_t payload_size = infs_le32_to_cpu(hdr->payload_size);
    if (memcmp(hdr->magic, INFS_OBJECT_MAGIC, 8) != 0 ||
        !object_type_valid(object_type) ||
        infs_le16_to_cpu(hdr->object_version) != 1u ||
        infs_le32_to_cpu(hdr->header_size) != sizeof(*hdr) ||
        infs_le64_to_cpu(hdr->generation) == 0 ||
        !id_is_nonzero(hdr->object_id) ||
        payload_size > INFS_BLOCK_SIZE - sizeof(*hdr) ||
        infs_le32_to_cpu(hdr->checksum_type) != INFS_CHECKSUM_CRC64_ECMA) {
        return INFS_STATUS_INVALID_ARGUMENT;
    }

    memset(block + sizeof(*hdr) + payload_size, 0,
           INFS_BLOCK_SIZE - sizeof(*hdr) - payload_size);
    const size_t off = offsetof(struct infs_object_header_disk, checksum);
    memset(block + off, 0, sizeof(hdr->checksum));
    uint64_t crc = infs_cpu_to_le64(block_crc_with_zeroed_checksum(
        block, off, sizeof(hdr->checksum)));
    memcpy(block + off, &crc, sizeof(crc));
    return INFS_STATUS_OK;
}

int infs_validate_object_block(const uint8_t block[INFS_BLOCK_SIZE])
{
    const struct infs_object_header_disk *hdr =
        (const struct infs_object_header_disk *)block;
    uint16_t object_type = infs_le16_to_cpu(hdr->object_type);
    uint32_t payload_size = infs_le32_to_cpu(hdr->payload_size);
    if (memcmp(hdr->magic, INFS_OBJECT_MAGIC, 8) != 0)
        return 0;
    if (!object_type_valid(object_type))
        return 0;
    if (infs_le16_to_cpu(hdr->object_version) != 1u)
        return 0;
    if (infs_le32_to_cpu(hdr->header_size) != sizeof(*hdr))
        return 0;
    if (infs_le64_to_cpu(hdr->generation) == 0)
        return 0;
    if (!id_is_nonzero(hdr->object_id))
        return 0;
    if (payload_size > INFS_BLOCK_SIZE - sizeof(*hdr))
        return 0;
    if (infs_le32_to_cpu(hdr->checksum_type) != INFS_CHECKSUM_CRC64_ECMA)
        return 0;
    if (!bytes_are_zero(hdr->checksum + sizeof(uint64_t),
                        sizeof(hdr->checksum) - sizeof(uint64_t)) ||
        !bytes_are_zero(block + sizeof(*hdr) + payload_size,
                        INFS_BLOCK_SIZE - sizeof(*hdr) - payload_size))
        return 0;

    const size_t off = offsetof(struct infs_object_header_disk, checksum);
    uint64_t stored_le;
    memcpy(&stored_le, block + off, sizeof(stored_le));
    uint64_t stored = infs_le64_to_cpu(stored_le);
    uint64_t actual = block_crc_with_zeroed_checksum(
        block, off, sizeof(hdr->checksum));
    return stored == actual;
}

static void fill_attributes(struct infs_attributes_disk *attributes,
                            uint64_t link_count, int64_t now_ns)
{
    memset(attributes, 0, sizeof(*attributes));
    attributes->logical_size = infs_cpu_to_le64(0);
    attributes->link_count = infs_cpu_to_le64(link_count);
    attributes->birth_time_ns = (int64_t)infs_cpu_to_le64((uint64_t)now_ns);
    attributes->access_time_ns = (int64_t)infs_cpu_to_le64((uint64_t)now_ns);
    attributes->modification_time_ns = (int64_t)infs_cpu_to_le64((uint64_t)now_ns);
    attributes->change_time_ns = (int64_t)infs_cpu_to_le64((uint64_t)now_ns);
}

infs_status infs_encode_root_directory(uint8_t block[INFS_BLOCK_SIZE],
                                       const uint8_t object_id[16],
                                       uint64_t generation,
                                       uint32_t permissions,
                                       uint32_t uid, uint32_t gid,
                                       int64_t now_ns)
{
    infs_status status = infs_object_init(
        block, INFS_OBJECT_DIRECTORY, object_id, NULL, generation,
        sizeof(struct infs_directory_payload_disk));
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_directory_payload_disk *payload =
        (struct infs_directory_payload_disk *)(block + sizeof(struct infs_object_header_disk));
    fill_attributes(&payload->attributes, 2u, now_ns);
    payload->posix.permissions = infs_cpu_to_le32(permissions & 07777u);
    payload->posix.uid = infs_cpu_to_le32(uid);
    payload->posix.gid = infs_cpu_to_le32(gid);
    payload->entry_count = infs_cpu_to_le32(0);
    payload->bytes_used = infs_cpu_to_le32(0);
    return infs_object_finalize(block);
}

infs_status infs_encode_object_index(uint8_t block[INFS_BLOCK_SIZE],
                                     const uint8_t object_id[16],
                                     uint64_t generation)
{
    infs_status status = infs_object_init(
        block, INFS_OBJECT_INDEX, object_id, NULL, generation,
        sizeof(struct infs_index_payload_disk));
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_index_payload_disk *payload =
        (struct infs_index_payload_disk *)(block + sizeof(struct infs_object_header_disk));
    payload->entry_count = infs_cpu_to_le32(0);
    payload->reserved = 0;
    return infs_object_finalize(block);
}

infs_status infs_read_best_superblock(const struct infs_storage *storage,
                                      uint64_t size_bytes,
                                      struct infs_superblock_disk *out,
                                      unsigned *valid_copies)
{
    if (!infs_storage_valid(storage) ||
        size_bytes < INFS_BLOCK_SIZE * 3u || !out)
        return INFS_STATUS_INVALID_ARGUMENT;

    const uint64_t total_blocks = size_bytes / INFS_BLOCK_SIZE;
    const uint64_t candidates[INFS_CHECKPOINT_COUNT] = {
        0,
        total_blocks / 2u,
        total_blocks - 1u
    };

    uint64_t best_generation = 0;
    int found = 0;
    unsigned valid = 0;
    uint8_t block[INFS_BLOCK_SIZE];

    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        uint64_t offset = candidates[i] * INFS_BLOCK_SIZE;
        if (infs_storage_read(storage, offset, block, sizeof(block)) !=
            INFS_STATUS_OK)
            continue;
        if (!infs_validate_superblock_block(block))
            continue;

        struct infs_superblock_disk sb;
        memcpy(&sb, block, sizeof(sb));
        if (infs_le64_to_cpu(sb.total_blocks) != total_blocks)
            continue;
        if (infs_le64_to_cpu(sb.checkpoint_block[0]) != candidates[0] ||
            infs_le64_to_cpu(sb.checkpoint_block[1]) != candidates[1] ||
            infs_le64_to_cpu(sb.checkpoint_block[2]) != candidates[2])
            continue;

        ++valid;
        uint64_t gen = infs_le64_to_cpu(sb.generation);
        if (!found || gen > best_generation) {
            memcpy(out, &sb, sizeof(*out));
            best_generation = gen;
            found = 1;
        }
    }

    if (valid_copies)
        *valid_copies = valid;
    if (!found)
        return INFS_STATUS_CORRUPT;
    return INFS_STATUS_OK;
}

void infs_uuid_to_string(const uint8_t id[16], char out[37])
{
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
             id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
}
