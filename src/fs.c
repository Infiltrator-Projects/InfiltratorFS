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
           type == INFS_OBJECT_INDEX || type == INFS_OBJECT_CHECKSUM ||
           type == INFS_OBJECT_SYMLINK ||
           type == INFS_OBJECT_SNAPSHOT_CATALOG;
}

static int object_version_valid(uint16_t type, uint16_t version)
{
    if (version == INFS_OBJECT_VERSION_CLASSIC)
        return 1;
    if (version == INFS_OBJECT_VERSION_PAGED)
        return type == INFS_OBJECT_DIRECTORY || type == INFS_OBJECT_INDEX ||
            type == INFS_OBJECT_FILE;
    return version == INFS_OBJECT_VERSION_TREE &&
        (type == INFS_OBJECT_INDEX || type == INFS_OBJECT_DIRECTORY);
}

static int index_payload_shape_valid(const uint8_t block[INFS_BLOCK_SIZE],
                                     uint32_t payload_size,
                                     uint16_t object_version)
{
    if (payload_size < sizeof(struct infs_index_payload_disk))
        return 0;
    const struct infs_index_payload_disk *payload =
        (const struct infs_index_payload_disk *)(
            block + sizeof(struct infs_object_header_disk));
    uint32_t count = infs_le32_to_cpu(payload->entry_count);

    if (object_version == INFS_OBJECT_VERSION_CLASSIC) {
        const size_t max_entries =
            (INFS_BLOCK_SIZE - sizeof(struct infs_object_header_disk) -
             sizeof(*payload)) / sizeof(struct infs_index_entry_disk);
        if (infs_le32_to_cpu(payload->reserved) != 0 ||
            (size_t)count > max_entries)
            return 0;
        size_t expected = sizeof(*payload) +
            (size_t)count * sizeof(struct infs_index_entry_disk);
        return expected == payload_size;
    }

    if (object_version == INFS_OBJECT_VERSION_TREE) {
        if (infs_le32_to_cpu(payload->reserved) != 0 || count == 0 ||
            payload_size != sizeof(*payload) + sizeof(uint64_t))
            return 0;
        uint64_t root_le = 0;
        memcpy(&root_le, payload + 1, sizeof(root_le));
        return infs_le64_to_cpu(root_le) != 0;
    }

    uint32_t page_count = infs_le32_to_cpu(payload->reserved);
    if (page_count > INFS_INDEX_PAGE_POINTERS ||
        (count == 0 && page_count != 0) ||
        (count != 0 && page_count == 0) ||
        (uint64_t)count >
            (uint64_t)page_count * INFS_INDEX_ENTRIES_PER_PAGE)
        return 0;
    size_t expected = sizeof(*payload) + (size_t)page_count * sizeof(uint64_t);
    return expected == payload_size;
}

static int symlink_payload_shape_valid(const uint8_t block[INFS_BLOCK_SIZE],
                                       uint32_t payload_size)
{
    if (payload_size < sizeof(struct infs_symlink_payload_disk))
        return 0;
    const struct infs_symlink_payload_disk *payload =
        (const struct infs_symlink_payload_disk *)(
            block + sizeof(struct infs_object_header_disk));
    uint32_t length = infs_le32_to_cpu(payload->target_length);
    if (length == 0 || length > INFS_SYMLINK_TARGET_MAX ||
        infs_le32_to_cpu(payload->reserved) != 0 ||
        payload_size != sizeof(*payload) + length ||
        infs_le64_to_cpu(payload->attributes.logical_size) != length)
        return 0;
    const uint8_t *target = (const uint8_t *)(payload + 1);
    return memchr(target, '\0', length) == NULL &&
        infs_utf8_validate(target, length);
}

static int snapshot_catalog_payload_shape_valid(
    const uint8_t block[INFS_BLOCK_SIZE], uint32_t payload_size)
{
    if (payload_size < sizeof(struct infs_snapshot_catalog_payload_disk))
        return 0;
    const struct infs_snapshot_catalog_payload_disk *payload =
        (const struct infs_snapshot_catalog_payload_disk *)(
            block + sizeof(struct infs_object_header_disk));
    uint32_t count = infs_le32_to_cpu(payload->snapshot_count);
    if (infs_le32_to_cpu(payload->reserved) != 0 ||
        count > INFS_SNAPSHOTS_PER_CATALOG)
        return 0;
    return payload_size == sizeof(*payload) +
        (size_t)count * sizeof(struct infs_snapshot_record_disk);
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
    if (infs_le16_to_cpu(sb->format_minor) != INFS_FORMAT_MINOR)
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
    const uint64_t required_flags =
        INFS_INCOMPAT_UTF8_NAMES | INFS_INCOMPAT_SPARSE_EXTENTS;
    if ((incompat_flags & ~INFS_KNOWN_INCOMPAT_FLAGS) != 0 ||
        (incompat_flags & required_flags) != required_flags)
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
    hdr->object_version = infs_cpu_to_le16(INFS_OBJECT_VERSION_CLASSIC);
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
    uint16_t object_version = infs_le16_to_cpu(hdr->object_version);
    uint32_t payload_size = infs_le32_to_cpu(hdr->payload_size);
    if (memcmp(hdr->magic, INFS_OBJECT_MAGIC, 8) != 0 ||
        !object_type_valid(object_type) ||
        !object_version_valid(object_type, object_version) ||
        infs_le32_to_cpu(hdr->header_size) != sizeof(*hdr) ||
        infs_le64_to_cpu(hdr->generation) == 0 ||
        !id_is_nonzero(hdr->object_id) ||
        payload_size > INFS_BLOCK_SIZE - sizeof(*hdr) ||
        infs_le32_to_cpu(hdr->checksum_type) != INFS_CHECKSUM_CRC64_ECMA ||
        (object_type == INFS_OBJECT_INDEX &&
         !index_payload_shape_valid(block, payload_size, object_version)) ||
        (object_type == INFS_OBJECT_SYMLINK &&
         !symlink_payload_shape_valid(block, payload_size)) ||
        (object_type == INFS_OBJECT_SNAPSHOT_CATALOG &&
         !snapshot_catalog_payload_shape_valid(block, payload_size))) {
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
    uint16_t object_version = infs_le16_to_cpu(hdr->object_version);
    uint32_t payload_size = infs_le32_to_cpu(hdr->payload_size);
    if (memcmp(hdr->magic, INFS_OBJECT_MAGIC, 8) != 0)
        return 0;
    if (!object_type_valid(object_type) ||
        !object_version_valid(object_type, object_version))
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
    if (object_type == INFS_OBJECT_INDEX &&
        !index_payload_shape_valid(block, payload_size, object_version))
        return 0;
    if (object_type == INFS_OBJECT_SYMLINK &&
        !symlink_payload_shape_valid(block, payload_size))
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

infs_status infs_metadata_page_init(uint8_t block[INFS_BLOCK_SIZE],
                                    const uint8_t magic[8],
                                    const uint8_t owner_object_id[16],
                                    uint64_t generation)
{
    if (!block || !magic || !owner_object_id ||
        !id_is_nonzero(owner_object_id) || generation == 0)
        return INFS_STATUS_INVALID_ARGUMENT;
    memset(block, 0, INFS_BLOCK_SIZE);
    struct infs_metadata_page_disk *page =
        (struct infs_metadata_page_disk *)block;
    memcpy(page->magic, magic, 8);
    page->generation = infs_cpu_to_le64(generation);
    memcpy(page->owner_object_id, owner_object_id, 16);
    page->checksum_type = infs_cpu_to_le32(INFS_CHECKSUM_CRC64_ECMA);
    return INFS_STATUS_OK;
}

infs_status infs_metadata_page_finalize(uint8_t block[INFS_BLOCK_SIZE])
{
    if (!block)
        return INFS_STATUS_INVALID_ARGUMENT;
    struct infs_metadata_page_disk *page =
        (struct infs_metadata_page_disk *)block;
    uint32_t bytes_used = infs_le32_to_cpu(page->bytes_used);
    if ((memcmp(page->magic, INFS_DIRECTORY_PAGE_MAGIC, 8) != 0 &&
         memcmp(page->magic, INFS_DIRECTORY_BRANCH_PAGE_MAGIC, 8) != 0 &&
         memcmp(page->magic, INFS_INDEX_PAGE_MAGIC, 8) != 0 &&
         memcmp(page->magic, INFS_INDEX_BRANCH_PAGE_MAGIC, 8) != 0 &&
         memcmp(page->magic, INFS_EXTENT_PAGE_MAGIC, 8) != 0) ||
        infs_le64_to_cpu(page->generation) == 0 ||
        !id_is_nonzero(page->owner_object_id) ||
        bytes_used > INFS_METADATA_PAGE_DATA_SIZE ||
        infs_le32_to_cpu(page->checksum_type) != INFS_CHECKSUM_CRC64_ECMA ||
        infs_le32_to_cpu(page->reserved) != 0)
        return INFS_STATUS_INVALID_ARGUMENT;

    memset(block + sizeof(*page) + bytes_used, 0,
           INFS_BLOCK_SIZE - sizeof(*page) - bytes_used);
    const size_t off = offsetof(struct infs_metadata_page_disk, checksum);
    memset(block + off, 0, sizeof(page->checksum));
    uint64_t crc = infs_cpu_to_le64(block_crc_with_zeroed_checksum(
        block, off, sizeof(page->checksum)));
    memcpy(block + off, &crc, sizeof(crc));
    return INFS_STATUS_OK;
}

int infs_validate_metadata_page(const uint8_t block[INFS_BLOCK_SIZE],
                                const uint8_t magic[8],
                                const uint8_t owner_object_id[16])
{
    if (!block || !magic || !owner_object_id)
        return 0;
    const struct infs_metadata_page_disk *page =
        (const struct infs_metadata_page_disk *)block;
    uint32_t bytes_used = infs_le32_to_cpu(page->bytes_used);
    if (memcmp(page->magic, magic, 8) != 0 ||
        infs_le64_to_cpu(page->generation) == 0 ||
        !id_is_nonzero(page->owner_object_id) ||
        memcmp(page->owner_object_id, owner_object_id, 16) != 0 ||
        bytes_used > INFS_METADATA_PAGE_DATA_SIZE ||
        infs_le32_to_cpu(page->checksum_type) != INFS_CHECKSUM_CRC64_ECMA ||
        infs_le32_to_cpu(page->reserved) != 0 ||
        !bytes_are_zero(page->checksum + sizeof(uint64_t),
                        sizeof(page->checksum) - sizeof(uint64_t)) ||
        !bytes_are_zero(block + sizeof(*page) + bytes_used,
                        INFS_BLOCK_SIZE - sizeof(*page) - bytes_used))
        return 0;

    const size_t off = offsetof(struct infs_metadata_page_disk, checksum);
    uint64_t stored_le;
    memcpy(&stored_le, block + off, sizeof(stored_le));
    uint64_t stored = infs_le64_to_cpu(stored_le);
    uint64_t actual = block_crc_with_zeroed_checksum(
        block, off, sizeof(page->checksum));
    return stored == actual;
}

infs_status infs_allocation_page_init(uint8_t block[INFS_BLOCK_SIZE],
                                      const uint8_t magic[8],
                                      uint64_t generation,
                                      uint64_t logical_index,
                                      uint32_t level)
{
    if (!block || !magic || generation == 0 ||
        level > INFS_ALLOCATION_TREE_ROOT_LEVEL ||
        (level == 0 &&
         memcmp(magic, INFS_ALLOCATION_LEAF_PAGE_MAGIC, 8) != 0) ||
        (level != 0 &&
         memcmp(magic, INFS_ALLOCATION_BRANCH_PAGE_MAGIC, 8) != 0))
        return INFS_STATUS_INVALID_ARGUMENT;
    memset(block, 0, INFS_BLOCK_SIZE);
    struct infs_allocation_page_disk *page =
        (struct infs_allocation_page_disk *)block;
    memcpy(page->magic, magic, 8);
    page->generation = infs_cpu_to_le64(generation);
    page->logical_index = infs_cpu_to_le64(logical_index);
    page->level = infs_cpu_to_le32(level);
    page->checksum_type = infs_cpu_to_le32(INFS_CHECKSUM_CRC64_ECMA);
    return INFS_STATUS_OK;
}

infs_status infs_allocation_page_finalize(uint8_t block[INFS_BLOCK_SIZE])
{
    if (!block)
        return INFS_STATUS_INVALID_ARGUMENT;
    struct infs_allocation_page_disk *page =
        (struct infs_allocation_page_disk *)block;
    uint32_t level = infs_le32_to_cpu(page->level);
    uint32_t entries = infs_le32_to_cpu(page->entry_count);
    uint32_t bytes = infs_le32_to_cpu(page->bytes_used);
    int leaf = level == 0;

    if (infs_le64_to_cpu(page->generation) == 0 ||
        level > INFS_ALLOCATION_TREE_ROOT_LEVEL ||
        (leaf && memcmp(page->magic, INFS_ALLOCATION_LEAF_PAGE_MAGIC, 8) != 0) ||
        (!leaf && memcmp(page->magic, INFS_ALLOCATION_BRANCH_PAGE_MAGIC, 8) != 0) ||
        infs_le32_to_cpu(page->checksum_type) != INFS_CHECKSUM_CRC64_ECMA)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (leaf) {
        if (entries == 0 || entries > INFS_ALLOCATION_BITS_PER_LEAF ||
            bytes != (entries + 7u) / 8u)
            return INFS_STATUS_INVALID_ARGUMENT;
    } else if (entries == 0 || entries > INFS_ALLOCATION_TREE_FANOUT ||
               bytes != entries * sizeof(uint64_t)) {
        return INFS_STATUS_INVALID_ARGUMENT;
    }
    if (bytes > INFS_ALLOCATION_PAGE_DATA_SIZE)
        return INFS_STATUS_INVALID_ARGUMENT;

    memset(block + sizeof(*page) + bytes, 0,
           INFS_BLOCK_SIZE - sizeof(*page) - bytes);
    const size_t off = offsetof(struct infs_allocation_page_disk, checksum);
    memset(block + off, 0, sizeof(page->checksum));
    uint64_t crc = infs_cpu_to_le64(block_crc_with_zeroed_checksum(
        block, off, sizeof(page->checksum)));
    memcpy(block + off, &crc, sizeof(crc));
    return INFS_STATUS_OK;
}

int infs_validate_allocation_page(const uint8_t block[INFS_BLOCK_SIZE],
                                  const uint8_t magic[8],
                                  uint64_t max_generation,
                                  uint64_t logical_index,
                                  uint32_t level)
{
    if (!block || !magic || max_generation == 0 ||
        level > INFS_ALLOCATION_TREE_ROOT_LEVEL)
        return 0;
    const struct infs_allocation_page_disk *page =
        (const struct infs_allocation_page_disk *)block;
    uint64_t generation = infs_le64_to_cpu(page->generation);
    uint32_t entries = infs_le32_to_cpu(page->entry_count);
    uint32_t bytes = infs_le32_to_cpu(page->bytes_used);
    int leaf = level == 0;

    if (memcmp(page->magic, magic, 8) != 0 ||
        generation == 0 || generation > max_generation ||
        infs_le64_to_cpu(page->logical_index) != logical_index ||
        infs_le32_to_cpu(page->level) != level ||
        infs_le32_to_cpu(page->checksum_type) != INFS_CHECKSUM_CRC64_ECMA)
        return 0;
    if (leaf) {
        if (entries == 0 || entries > INFS_ALLOCATION_BITS_PER_LEAF ||
            bytes != (entries + 7u) / 8u)
            return 0;
    } else if (entries == 0 || entries > INFS_ALLOCATION_TREE_FANOUT ||
               bytes != entries * sizeof(uint64_t)) {
        return 0;
    }
    if (bytes > INFS_ALLOCATION_PAGE_DATA_SIZE ||
        !bytes_are_zero(page->checksum + sizeof(uint64_t),
                        sizeof(page->checksum) - sizeof(uint64_t)) ||
        !bytes_are_zero(block + sizeof(*page) + bytes,
                        INFS_BLOCK_SIZE - sizeof(*page) - bytes))
        return 0;
    const size_t off = offsetof(struct infs_allocation_page_disk, checksum);
    uint64_t stored_le;
    memcpy(&stored_le, block + off, sizeof(stored_le));
    uint64_t stored = infs_le64_to_cpu(stored_le);
    uint64_t actual = block_crc_with_zeroed_checksum(
        block, off, sizeof(page->checksum));
    return stored == actual;
}

static void fill_attributes(struct infs_attributes_disk *attributes,
                            uint64_t link_count,
                            const struct infs_timestamp *now)
{
    memset(attributes, 0, sizeof(*attributes));
    attributes->logical_size = infs_cpu_to_le64(0);
    attributes->link_count = infs_cpu_to_le64(link_count);
    attributes->birth_time.seconds = (int64_t)infs_cpu_to_le64((uint64_t)now->seconds);
    attributes->birth_time.nanoseconds = infs_cpu_to_le32(now->nanoseconds);
    attributes->access_time = attributes->birth_time;
    attributes->modification_time = attributes->birth_time;
    attributes->change_time = attributes->birth_time;
}

static infs_status encode_root_directory_version(
    uint8_t block[INFS_BLOCK_SIZE], const uint8_t object_id[16],
    uint64_t generation, uint32_t permissions, uint32_t uid, uint32_t gid,
    const struct infs_timestamp *now, uint16_t object_version)
{
    infs_status status = infs_object_init(
        block, INFS_OBJECT_DIRECTORY, object_id, NULL, generation,
        sizeof(struct infs_directory_payload_disk));
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)block;
    header->object_version = infs_cpu_to_le16(object_version);
    struct infs_directory_payload_disk *payload =
        (struct infs_directory_payload_disk *)(block + sizeof(*header));
    fill_attributes(&payload->attributes, 2u, now);
    payload->posix.permissions = infs_cpu_to_le32(permissions & 07777u);
    payload->posix.uid = infs_cpu_to_le32(uid);
    payload->posix.gid = infs_cpu_to_le32(gid);
    payload->entry_count = infs_cpu_to_le32(0);
    payload->bytes_used = infs_cpu_to_le32(0);
    return infs_object_finalize(block);
}

infs_status infs_encode_root_directory(uint8_t block[INFS_BLOCK_SIZE],
                                       const uint8_t object_id[16],
                                       uint64_t generation,
                                       uint32_t permissions,
                                       uint32_t uid, uint32_t gid,
                                       const struct infs_timestamp *now)
{
    return encode_root_directory_version(
        block, object_id, generation, permissions, uid, gid, now,
        INFS_OBJECT_VERSION_CLASSIC);
}

infs_status infs_encode_paged_root_directory(uint8_t block[INFS_BLOCK_SIZE],
                                             const uint8_t object_id[16],
                                             uint64_t generation,
                                             uint32_t permissions,
                                             uint32_t uid, uint32_t gid,
                                             const struct infs_timestamp *now)
{
    return encode_root_directory_version(
        block, object_id, generation, permissions, uid, gid, now,
        INFS_OBJECT_VERSION_PAGED);
}

infs_status infs_encode_tree_root_directory(uint8_t block[INFS_BLOCK_SIZE],
                                            const uint8_t object_id[16],
                                            uint64_t generation,
                                            uint32_t permissions,
                                            uint32_t uid, uint32_t gid,
                                            const struct infs_timestamp *now)
{
    infs_status status = encode_root_directory_version(
        block, object_id, generation, permissions, uid, gid, now,
        INFS_OBJECT_VERSION_TREE);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)block;
    struct infs_directory_payload_disk *payload =
        (struct infs_directory_payload_disk *)(header + 1);
    uint64_t root_le = infs_cpu_to_le64(0);

    payload->entry_count = infs_cpu_to_le32(0);
    payload->bytes_used = infs_cpu_to_le32(0);
    memcpy(payload + 1, &root_le, sizeof(root_le));
    header->payload_size = infs_cpu_to_le32(
        (uint32_t)(sizeof(*payload) + sizeof(root_le)));
    return infs_object_finalize(block);
}

static infs_status encode_object_index_version(
    uint8_t block[INFS_BLOCK_SIZE], const uint8_t object_id[16],
    uint64_t generation, uint16_t object_version)
{
    infs_status status = infs_object_init(
        block, INFS_OBJECT_INDEX, object_id, NULL, generation,
        sizeof(struct infs_index_payload_disk));
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)block;
    header->object_version = infs_cpu_to_le16(object_version);
    struct infs_index_payload_disk *payload =
        (struct infs_index_payload_disk *)(block + sizeof(*header));
    payload->entry_count = infs_cpu_to_le32(0);
    payload->reserved = infs_cpu_to_le32(0);
    return infs_object_finalize(block);
}

infs_status infs_encode_object_index(uint8_t block[INFS_BLOCK_SIZE],
                                     const uint8_t object_id[16],
                                     uint64_t generation)
{
    return encode_object_index_version(
        block, object_id, generation, INFS_OBJECT_VERSION_CLASSIC);
}

infs_status infs_encode_paged_object_index(uint8_t block[INFS_BLOCK_SIZE],
                                           const uint8_t object_id[16],
                                           uint64_t generation)
{
    return encode_object_index_version(
        block, object_id, generation, INFS_OBJECT_VERSION_PAGED);
}

infs_status infs_encode_tree_object_index(uint8_t block[INFS_BLOCK_SIZE],
                                          const uint8_t object_id[16],
                                          uint64_t generation,
                                          uint64_t root_node_block,
                                          uint32_t entry_count)
{
    if (!block || !object_id || !root_node_block || !entry_count)
        return INFS_STATUS_INVALID_ARGUMENT;

    infs_status status = infs_object_init(
        block, INFS_OBJECT_INDEX, object_id, NULL, generation,
        (uint32_t)(sizeof(struct infs_index_payload_disk) + sizeof(uint64_t)));
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)block;
    struct infs_index_payload_disk *payload =
        (struct infs_index_payload_disk *)(header + 1);
    uint64_t root_le = infs_cpu_to_le64(root_node_block);

    header->object_version = infs_cpu_to_le16(INFS_OBJECT_VERSION_TREE);
    payload->entry_count = infs_cpu_to_le32(entry_count);
    payload->reserved = infs_cpu_to_le32(0);
    memcpy(payload + 1, &root_le, sizeof(root_le));
    header->payload_size = infs_cpu_to_le32(
        (uint32_t)(sizeof(*payload) + sizeof(root_le)));
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

    const uint64_t storage_blocks = size_bytes / INFS_BLOCK_SIZE;
    uint64_t candidates[INFS_CHECKPOINT_COUNT] = {
        0, storage_blocks / 2u, storage_blocks - 1u
    };
    struct infs_superblock_disk anchor;
    int anchored = 0;
    uint8_t block[INFS_BLOCK_SIZE];

    /*
     * Block zero is the stable geometry anchor.  This lets a filesystem be
     * smaller than its backing partition while an online grow (or staged
     * shrink) is in progress.  Existing unresized volumes keep the historical
     * half/end fallback when block zero is unreadable.
     */
    if (infs_storage_read(storage, 0, block, sizeof(block)) ==
            INFS_STATUS_OK &&
        infs_validate_superblock_block(block)) {
        memcpy(&anchor, block, sizeof(anchor));
        uint64_t total = infs_le64_to_cpu(anchor.total_blocks);
        uint64_t c0 = infs_le64_to_cpu(anchor.checkpoint_block[0]);
        uint64_t c1 = infs_le64_to_cpu(anchor.checkpoint_block[1]);
        uint64_t c2 = infs_le64_to_cpu(anchor.checkpoint_block[2]);
        if (total >= 3u && total <= storage_blocks &&
            c0 == 0 && c1 < total && c2 < total &&
            c1 != c0 && c2 != c0 && c2 != c1) {
            candidates[0] = c0;
            candidates[1] = c1;
            candidates[2] = c2;
            anchored = 1;
        }
    }

    uint64_t best_generation = 0;
    int found = 0;
    unsigned valid = 0;

    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        uint64_t offset = candidates[i] * INFS_BLOCK_SIZE;
        if (infs_storage_read(storage, offset, block, sizeof(block)) !=
            INFS_STATUS_OK)
            continue;
        if (!infs_validate_superblock_block(block))
            continue;

        struct infs_superblock_disk sb;
        memcpy(&sb, block, sizeof(sb));
        uint64_t total = infs_le64_to_cpu(sb.total_blocks);
        if (total < 3u || total > storage_blocks ||
            infs_le64_to_cpu(sb.checkpoint_block[0]) != candidates[0] ||
            infs_le64_to_cpu(sb.checkpoint_block[1]) != candidates[1] ||
            infs_le64_to_cpu(sb.checkpoint_block[2]) != candidates[2])
            continue;
        if (anchored &&
            (total != infs_le64_to_cpu(anchor.total_blocks) ||
             memcmp(sb.filesystem_uuid, anchor.filesystem_uuid,
                    sizeof(sb.filesystem_uuid)) != 0))
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
