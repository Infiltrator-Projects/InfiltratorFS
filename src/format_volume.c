// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/format_volume.h"

#include "infilfs/endian.h"
#include "infilfs/format.h"
#include "infilfs/fs.h"
#include "infilfs/utf8.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define INFS_MIN_SIZE_BYTES (UINT64_C(16) * 1024u * 1024u)

static void formatter_bitmap_set(uint8_t *bitmap, uint64_t block)
{
    bitmap[block >> 3] |= (uint8_t)(1u << (block & 7u));
}

static int formatter_id_is_zero(const uint8_t id[16])
{
    uint8_t combined = 0;
    for (unsigned i = 0; i < 16u; ++i)
        combined |= id[i];
    return combined == 0;
}

static infs_status formatter_generate_id(struct infs_storage *storage,
                                         uint8_t id[16],
                                         const uint8_t avoid1[16],
                                         const uint8_t avoid2[16])
{
    for (unsigned attempt = 0; attempt < 32u; ++attempt) {
        infs_status status = infs_storage_random(storage, id, 16u);
        if (status != INFS_STATUS_OK)
            return status;
        if (formatter_id_is_zero(id))
            continue;
        if (avoid1 && memcmp(id, avoid1, 16u) == 0)
            continue;
        if (avoid2 && memcmp(id, avoid2, 16u) == 0)
            continue;
        return INFS_STATUS_OK;
    }
    return INFS_STATUS_IO_ERROR;
}

infs_status infs_format_storage(struct infs_storage *storage, const char *label)
{
    if (!storage || !infs_storage_valid(storage) || !storage->ops->write_at ||
        !storage->ops->flush || !storage->ops->random_bytes ||
        !storage->ops->current_time_ns || !label)
        return INFS_STATUS_INVALID_ARGUMENT;

    size_t label_length = strlen(label);
    if (label_length >= INFS_LABEL_MAX)
        return INFS_STATUS_NAME_TOO_LONG;
    if (!infs_utf8_validate(label, label_length))
        return INFS_STATUS_INVALID_ARGUMENT;

    uint64_t size_bytes = 0;
    int is_device = 0;
    infs_status status = infs_storage_get_size(storage, &size_bytes, &is_device);
    if (status != INFS_STATUS_OK)
        return status;
    (void)is_device;
    if (size_bytes < INFS_MIN_SIZE_BYTES)
        return INFS_STATUS_NO_SPACE;

    const uint64_t total_blocks = size_bytes / INFS_BLOCK_SIZE;
    const uint64_t bitmap_bytes = (total_blocks + 7u) / 8u;
    const uint64_t bitmap_blocks =
        (bitmap_bytes + INFS_BLOCK_SIZE - 1u) / INFS_BLOCK_SIZE;
    const uint64_t bitmap_start = 1u;
    const uint64_t index_block = bitmap_start + bitmap_blocks;
    const uint64_t root_block = index_block + 1u;
    const uint64_t index_page_block = root_block + 1u;
    const uint64_t checkpoints[INFS_CHECKPOINT_COUNT] = {
        0u, total_blocks / 2u, total_blocks - 1u
    };

    if (bitmap_blocks > SIZE_MAX / INFS_BLOCK_SIZE)
        return INFS_STATUS_OVERFLOW;
    if (index_page_block >= checkpoints[1])
        return INFS_STATUS_NO_SPACE;

    size_t bitmap_alloc = (size_t)(bitmap_blocks * INFS_BLOCK_SIZE);
    uint8_t *bitmap = calloc(1u, bitmap_alloc);
    if (!bitmap)
        return INFS_STATUS_NO_MEMORY;

    formatter_bitmap_set(bitmap, checkpoints[0]);
    formatter_bitmap_set(bitmap, checkpoints[1]);
    formatter_bitmap_set(bitmap, checkpoints[2]);
    for (uint64_t block = bitmap_start;
         block < bitmap_start + bitmap_blocks; ++block)
        formatter_bitmap_set(bitmap, block);
    formatter_bitmap_set(bitmap, index_block);
    formatter_bitmap_set(bitmap, root_block);
    formatter_bitmap_set(bitmap, index_page_block);
    for (uint64_t block = total_blocks;
         block < (uint64_t)bitmap_alloc * 8u; ++block)
        formatter_bitmap_set(bitmap, block);

    const uint64_t used_blocks = bitmap_blocks + 6u;
    uint8_t filesystem_uuid[16];
    uint8_t root_id[16];
    uint8_t index_id[16];
    status = formatter_generate_id(storage, filesystem_uuid, NULL, NULL);
    if (status == INFS_STATUS_OK)
        status = formatter_generate_id(storage, root_id, filesystem_uuid, NULL);
    if (status == INFS_STATUS_OK)
        status = formatter_generate_id(storage, index_id, filesystem_uuid, root_id);
    if (status != INFS_STATUS_OK) {
        free(bitmap);
        return status;
    }

    int64_t root_time_ns = 0;
    status = infs_storage_current_time_ns(storage, &root_time_ns);
    if (status != INFS_STATUS_OK) {
        free(bitmap);
        return status;
    }

    struct infs_superblock_disk sb;
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, INFS_MAGIC, 8u);
    sb.format_major = infs_cpu_to_le16(INFS_FORMAT_MAJOR);
    sb.format_minor = infs_cpu_to_le16(INFS_FORMAT_MINOR);
    sb.header_size = infs_cpu_to_le16((uint16_t)sizeof(sb));
    sb.block_shift = infs_cpu_to_le16(INFS_BLOCK_SHIFT);
    sb.checksum_type = infs_cpu_to_le32(INFS_CHECKSUM_CRC64_ECMA);
    sb.generation = infs_cpu_to_le64(1u);
    sb.total_blocks = infs_cpu_to_le64(total_blocks);
    sb.free_blocks = infs_cpu_to_le64(total_blocks - used_blocks);
    sb.bitmap_start_block = infs_cpu_to_le64(bitmap_start);
    sb.bitmap_block_count = infs_cpu_to_le64(bitmap_blocks);
    sb.object_index_block = infs_cpu_to_le64(index_block);
    sb.root_object_block = infs_cpu_to_le64(root_block);
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        sb.checkpoint_block[i] = infs_cpu_to_le64(checkpoints[i]);
    memcpy(sb.filesystem_uuid, filesystem_uuid, sizeof(filesystem_uuid));
    memcpy(sb.root_object_id, root_id, sizeof(root_id));
    sb.compat_flags = infs_cpu_to_le64(INFS_KNOWN_COMPAT_FLAGS);
    sb.ro_compat_flags = infs_cpu_to_le64(INFS_KNOWN_RO_COMPAT_FLAGS);
    sb.incompat_flags = infs_cpu_to_le64(
        INFS_INCOMPAT_UTF8_NAMES | INFS_INCOMPAT_SPARSE_EXTENTS |
        INFS_INCOMPAT_INLINE_DATA | INFS_INCOMPAT_SHARED_EXTENTS |
        INFS_INCOMPAT_PAGED_METADATA | INFS_INCOMPAT_SYMBOLIC_LINKS |
        INFS_INCOMPAT_HARD_LINKS);
    memcpy(sb.label, label, label_length);

    uint8_t block[INFS_BLOCK_SIZE] = {0};
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        status = infs_storage_write(storage,
            checkpoints[i] * INFS_BLOCK_SIZE, block, sizeof(block));
        if (status != INFS_STATUS_OK)
            goto done;
    }
    status = infs_storage_flush(storage);
    if (status != INFS_STATUS_OK)
        goto done;

    status = infs_storage_write(storage, bitmap_start * INFS_BLOCK_SIZE,
                                bitmap, bitmap_alloc);
    if (status != INFS_STATUS_OK)
        goto done;

    status = infs_metadata_page_init(block, INFS_INDEX_PAGE_MAGIC, index_id, 1u);
    if (status != INFS_STATUS_OK)
        goto done;
    struct infs_metadata_page_disk *index_page =
        (struct infs_metadata_page_disk *)block;
    struct infs_index_entry_disk *entry =
        (struct infs_index_entry_disk *)(index_page + 1);
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->object_id, root_id, 16u);
    entry->object_block = infs_cpu_to_le64(root_block);
    entry->object_type = infs_cpu_to_le16(INFS_OBJECT_DIRECTORY);
    index_page->entry_count = infs_cpu_to_le32(1u);
    index_page->bytes_used = infs_cpu_to_le32(sizeof(*entry));
    status = infs_metadata_page_finalize(block);
    if (status != INFS_STATUS_OK)
        goto done;
    status = infs_storage_write(storage, index_page_block * INFS_BLOCK_SIZE,
                                block, sizeof(block));
    if (status != INFS_STATUS_OK)
        goto done;

    status = infs_encode_paged_object_index(block, index_id, 1u);
    if (status != INFS_STATUS_OK)
        goto done;
    struct infs_object_header_disk *index_header =
        (struct infs_object_header_disk *)block;
    struct infs_index_payload_disk *index_payload =
        (struct infs_index_payload_disk *)(block + sizeof(*index_header));
    uint64_t *page_pointer = (uint64_t *)(index_payload + 1);
    index_payload->entry_count = infs_cpu_to_le32(1u);
    index_payload->reserved = infs_cpu_to_le32(1u);
    page_pointer[0] = infs_cpu_to_le64(index_page_block);
    index_header->payload_size = infs_cpu_to_le32(
        (uint32_t)(sizeof(*index_payload) + sizeof(*page_pointer)));
    status = infs_object_finalize(block);
    if (status != INFS_STATUS_OK)
        goto done;
    status = infs_storage_write(storage, index_block * INFS_BLOCK_SIZE,
                                block, sizeof(block));
    if (status != INFS_STATUS_OK)
        goto done;

    status = infs_encode_paged_root_directory(
        block, root_id, 1u, 0755u, 0u, 0u, root_time_ns);
    if (status != INFS_STATUS_OK)
        goto done;
    status = infs_storage_write(storage, root_block * INFS_BLOCK_SIZE,
                                block, sizeof(block));
    if (status != INFS_STATUS_OK)
        goto done;

    status = infs_storage_flush(storage);
    if (status != INFS_STATUS_OK)
        goto done;

    status = infs_encode_superblock(block, &sb);
    if (status != INFS_STATUS_OK)
        goto done;
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        status = infs_storage_write(storage,
            checkpoints[i] * INFS_BLOCK_SIZE, block, sizeof(block));
        if (status != INFS_STATUS_OK)
            goto done;
    }
    status = infs_storage_flush(storage);

done:
    free(bitmap);
    return status;
}
