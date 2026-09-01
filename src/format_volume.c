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

static infs_status formatter_allocation_counts(
    uint64_t total, uint64_t *leaves_out, uint64_t *level1_out,
    uint64_t *level2_out, uint64_t *branches_out)
{
    if (!total || total > INFS_ALLOCATION_TREE_MAX_BLOCKS)
        return INFS_STATUS_NOT_SUPPORTED;
    uint64_t leaves = total / INFS_ALLOCATION_BITS_PER_LEAF +
        ((total % INFS_ALLOCATION_BITS_PER_LEAF) != 0);
    uint64_t level1 = leaves / INFS_ALLOCATION_TREE_FANOUT +
        ((leaves % INFS_ALLOCATION_TREE_FANOUT) != 0);
    uint64_t level2 = level1 / INFS_ALLOCATION_TREE_FANOUT +
        ((level1 % INFS_ALLOCATION_TREE_FANOUT) != 0);
    if (!leaves || !level1 || !level2)
        return INFS_STATUS_CORRUPT;
    *leaves_out = leaves;
    *level1_out = level1;
    *level2_out = level2;
    *branches_out = UINT64_C(1) + level1 + level2;
    return INFS_STATUS_OK;
}

static infs_status formatter_write_allocation_page(
    struct infs_storage *storage, uint64_t physical_block,
    const uint8_t magic[8], uint64_t logical_index, uint32_t level,
    uint32_t entries, const void *payload, uint32_t bytes)
{
    uint8_t block[INFS_BLOCK_SIZE];
    infs_status status = infs_allocation_page_init(
        block, magic, 1u, logical_index, level);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_allocation_page_disk *page =
        (struct infs_allocation_page_disk *)block;
    page->entry_count = infs_cpu_to_le32(entries);
    page->bytes_used = infs_cpu_to_le32(bytes);
    if (bytes)
        memcpy(page + 1, payload, bytes);
    status = infs_allocation_page_finalize(block);
    if (status != INFS_STATUS_OK)
        return status;
    return infs_storage_write(storage, physical_block * INFS_BLOCK_SIZE,
                              block, sizeof(block));
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
    uint64_t leaf_count = 0, level1_count = 0, level2_count = 0;
    uint64_t branch_count = 0;
    status = formatter_allocation_counts(
        total_blocks, &leaf_count, &level1_count, &level2_count,
        &branch_count);
    if (status != INFS_STATUS_OK)
        return status;

    const uint64_t allocation_root = 1u;
    const uint64_t level2_start = allocation_root + 1u;
    const uint64_t level1_start = level2_start + level2_count;
    const uint64_t leaf_start = level1_start + level1_count;
    const uint64_t index_block = leaf_start + leaf_count;
    const uint64_t root_block = index_block + 1u;
    const uint64_t index_page_block = root_block + 1u;
    const uint64_t checkpoints[INFS_CHECKPOINT_COUNT] = {
        0u, total_blocks / 2u, total_blocks - 1u
    };

    if (index_page_block >= checkpoints[1])
        return INFS_STATUS_NO_SPACE;

    uint64_t raw_bitmap_bytes = (total_blocks + 7u) / 8u;
    uint64_t bitmap_blocks =
        (raw_bitmap_bytes + INFS_BLOCK_SIZE - 1u) / INFS_BLOCK_SIZE;
    if (!bitmap_blocks || bitmap_blocks > SIZE_MAX / INFS_BLOCK_SIZE)
        return INFS_STATUS_OVERFLOW;
    size_t bitmap_alloc = (size_t)(bitmap_blocks * INFS_BLOCK_SIZE);
    uint8_t *bitmap = calloc(1u, bitmap_alloc);
    if (!bitmap)
        return INFS_STATUS_NO_MEMORY;

    formatter_bitmap_set(bitmap, checkpoints[0]);
    formatter_bitmap_set(bitmap, checkpoints[1]);
    formatter_bitmap_set(bitmap, checkpoints[2]);
    for (uint64_t block = allocation_root;
         block < leaf_start + leaf_count; ++block)
        formatter_bitmap_set(bitmap, block);
    formatter_bitmap_set(bitmap, index_block);
    formatter_bitmap_set(bitmap, root_block);
    formatter_bitmap_set(bitmap, index_page_block);
    for (uint64_t block = total_blocks;
         block < (uint64_t)bitmap_alloc * 8u; ++block)
        formatter_bitmap_set(bitmap, block);

    uint64_t used_blocks = 0;
    for (uint64_t block = 0; block < total_blocks; ++block)
        if ((bitmap[block >> 3] >> (block & 7u)) & 1u)
            ++used_blocks;
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
    sb.allocation_root_block = infs_cpu_to_le64(allocation_root);
    sb.allocation_leaf_count = infs_cpu_to_le64(leaf_count);
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
        INFS_INCOMPAT_HARD_LINKS | INFS_INCOMPAT_SNAPSHOTS |
        INFS_INCOMPAT_PAGED_EXTENTS | INFS_INCOMPAT_INDEX_TREE |
        INFS_INCOMPAT_DIRECTORY_TREE | INFS_INCOMPAT_ALLOCATION_TREE |
        INFS_INCOMPAT_COMPRESSED_EXTENTS);
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

    for (uint64_t i = 0; i < leaf_count; ++i) {
        uint64_t first_bit = i * INFS_ALLOCATION_BITS_PER_LEAF;
        uint64_t remaining = total_blocks - first_bit;
        uint32_t valid_bits = (uint32_t)(
            remaining > INFS_ALLOCATION_BITS_PER_LEAF ?
            INFS_ALLOCATION_BITS_PER_LEAF : remaining);
        uint32_t bytes = (valid_bits + 7u) / 8u;
        size_t offset = (size_t)i * INFS_ALLOCATION_PAGE_DATA_SIZE;
        status = formatter_write_allocation_page(
            storage, leaf_start + i, INFS_ALLOCATION_LEAF_PAGE_MAGIC,
            i, 0u, valid_bits, bitmap + offset, bytes);
        if (status != INFS_STATUS_OK)
            goto done;
    }

    for (uint64_t i = 0; i < level1_count; ++i) {
        uint64_t first_leaf = i * INFS_ALLOCATION_TREE_FANOUT;
        uint32_t entries = (uint32_t)(
            leaf_count - first_leaf > INFS_ALLOCATION_TREE_FANOUT ?
            INFS_ALLOCATION_TREE_FANOUT : leaf_count - first_leaf);
        uint64_t pointers[INFS_ALLOCATION_TREE_FANOUT];
        for (uint32_t j = 0; j < entries; ++j)
            pointers[j] = infs_cpu_to_le64(leaf_start + first_leaf + j);
        status = formatter_write_allocation_page(
            storage, level1_start + i, INFS_ALLOCATION_BRANCH_PAGE_MAGIC,
            i, 1u, entries, pointers, entries * sizeof(uint64_t));
        if (status != INFS_STATUS_OK)
            goto done;
    }

    for (uint64_t i = 0; i < level2_count; ++i) {
        uint64_t first_l1 = i * INFS_ALLOCATION_TREE_FANOUT;
        uint32_t entries = (uint32_t)(
            level1_count - first_l1 > INFS_ALLOCATION_TREE_FANOUT ?
            INFS_ALLOCATION_TREE_FANOUT : level1_count - first_l1);
        uint64_t pointers[INFS_ALLOCATION_TREE_FANOUT];
        for (uint32_t j = 0; j < entries; ++j)
            pointers[j] = infs_cpu_to_le64(level1_start + first_l1 + j);
        status = formatter_write_allocation_page(
            storage, level2_start + i, INFS_ALLOCATION_BRANCH_PAGE_MAGIC,
            i, 2u, entries, pointers, entries * sizeof(uint64_t));
        if (status != INFS_STATUS_OK)
            goto done;
    }

    {
        uint64_t pointers[INFS_ALLOCATION_TREE_FANOUT];
        for (uint32_t i = 0; i < (uint32_t)level2_count; ++i)
            pointers[i] = infs_cpu_to_le64(level2_start + i);
        status = formatter_write_allocation_page(
            storage, allocation_root, INFS_ALLOCATION_BRANCH_PAGE_MAGIC,
            0u, INFS_ALLOCATION_TREE_ROOT_LEVEL, (uint32_t)level2_count,
            pointers, (uint32_t)level2_count * sizeof(uint64_t));
        if (status != INFS_STATUS_OK)
            goto done;
    }

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

    status = infs_encode_tree_object_index(
        block, index_id, 1u, index_page_block, 1u);
    if (status != INFS_STATUS_OK)
        goto done;
    status = infs_storage_write(storage, index_block * INFS_BLOCK_SIZE,
                                block, sizeof(block));
    if (status != INFS_STATUS_OK)
        goto done;

    status = infs_encode_tree_root_directory(
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
