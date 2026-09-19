// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"

const u8 infilfs_extent_index_page_magic[8] = {
    'I', 'N', 'F', 'S', 'E', 'I', '0', '1'
};

u32 infilfs_extent_pointer_tree_levels(u32 page_count)
{
    u64 capacity = INFILFS_EXTENT_INDEX_POINTERS_PER_PAGE;
    u32 levels = 1;

    if (!page_count)
        return 0;
    while (capacity < page_count && levels < INFILFS_EXTENT_INDEX_MAX_LEVELS) {
        if (capacity > U64_MAX / INFILFS_EXTENT_INDEX_POINTERS_PER_PAGE)
            return 0;
        capacity *= INFILFS_EXTENT_INDEX_POINTERS_PER_PAGE;
        ++levels;
    }
    return capacity >= page_count ? levels : 0;
}

bool infilfs_extent_pointer_page_valid(
    struct super_block *sb, const u8 block[INFILFS_DISK_BLOCK_SIZE],
    const u8 owner_id[16], u32 expected_level,
    const __le64 **pointers_out, u32 *count_out)
{
    const struct infilfs_metadata_page_disk *page =
        (const struct infilfs_metadata_page_disk *)block;
    const __le64 *pointers;
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    u32 count, bytes, i;

    if (!sbi ||
        memcmp(page->magic, infilfs_extent_index_page_magic, 8) != 0 ||
        memcmp(page->owner_object_id, owner_id, 16) != 0 ||
        !infilfs_crc64_block_valid(
            block, offsetof(struct infilfs_metadata_page_disk, checksum),
            sizeof(page->checksum)) ||
        le64_to_cpu(page->generation) == 0 ||
        le64_to_cpu(page->generation) > le64_to_cpu(sbi->disk.generation) ||
        le32_to_cpu(page->checksum_type) != INFILFS_CHECKSUM_CRC64_ECMA ||
        le32_to_cpu(page->reserved) != expected_level)
        return false;

    count = le32_to_cpu(page->entry_count);
    bytes = le32_to_cpu(page->bytes_used);
    if (!count || count > INFILFS_EXTENT_INDEX_POINTERS_PER_PAGE ||
        bytes != count * sizeof(__le64))
        return false;

    pointers = (const __le64 *)(page + 1);
    for (i = 0; i < count; ++i)
        if (!infilfs_block_allocated(sb, le64_to_cpu(pointers[i])))
            return false;

    if (pointers_out)
        *pointers_out = pointers;
    if (count_out)
        *count_out = count;
    return true;
}

int infilfs_extent_layout_validate(
    struct super_block *sb, const u8 object[INFILFS_DISK_BLOCK_SIZE],
    u32 *page_count_out)
{
    const struct infilfs_object_header_disk *header =
        (const struct infilfs_object_header_disk *)object;
    const struct infilfs_file_payload_disk *file =
        (const struct infilfs_file_payload_disk *)(header + 1);
    const struct infilfs_extent_head_disk *head =
        (const struct infilfs_extent_head_disk *)(file + 1);
    const __le64 *pointers = (const __le64 *)(head + 1);
    u16 version = le16_to_cpu(header->object_version);
    u32 page_count = le32_to_cpu(head->page_count);
    u32 extent_count = le32_to_cpu(file->extent_count);
    size_t expected;

    if (!page_count || !extent_count || page_count > extent_count)
        return -EFSCORRUPTED;

    if (version == INFILFS_OBJECT_VERSION_PAGED) {
        u32 p;
        if (page_count > INFILFS_EXTENT_PAGE_POINTERS ||
            le32_to_cpu(head->reserved) != 0)
            return -EFSCORRUPTED;
        expected = sizeof(*file) + sizeof(*head) +
            (size_t)page_count * sizeof(*pointers);
        if (expected != le32_to_cpu(header->payload_size))
            return -EFSCORRUPTED;
        for (p = 0; p < page_count; ++p)
            if (!infilfs_block_allocated(sb, le64_to_cpu(pointers[p])))
                return -EFSCORRUPTED;
    } else if (version == INFILFS_OBJECT_VERSION_TREE) {
        u32 levels = le32_to_cpu(head->reserved);
        u64 root = le64_to_cpu(pointers[0]);
        u8 *page;
        int ret = 0;

        if (page_count <= INFILFS_EXTENT_PAGE_POINTERS ||
            levels != infilfs_extent_pointer_tree_levels(page_count) ||
            !root || !infilfs_block_allocated(sb, root) ||
            le32_to_cpu(header->payload_size) !=
                sizeof(*file) + sizeof(*head) + sizeof(*pointers))
            return -EFSCORRUPTED;
        page = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!page)
            return -ENOMEM;
        ret = infilfs_read_allocated_block(sb, root, page);
        if (!ret && !infilfs_extent_pointer_page_valid(
                sb, page, header->object_id, levels - 1u, NULL, NULL))
            ret = -EFSCORRUPTED;
        kfree(page);
        if (ret)
            return ret;
    } else {
        return -EFSCORRUPTED;
    }

    if (page_count_out)
        *page_count_out = page_count;
    return 0;
}

int infilfs_extent_page_block(
    struct super_block *sb, const u8 object[INFILFS_DISK_BLOCK_SIZE],
    u32 page_index, u64 *block_out)
{
    const struct infilfs_object_header_disk *header =
        (const struct infilfs_object_header_disk *)object;
    const struct infilfs_file_payload_disk *file =
        (const struct infilfs_file_payload_disk *)(header + 1);
    const struct infilfs_extent_head_disk *head =
        (const struct infilfs_extent_head_disk *)(file + 1);
    const __le64 *head_pointers = (const __le64 *)(head + 1);
    u16 version = le16_to_cpu(header->object_version);
    u32 page_count;
    int ret;

    if (!block_out)
        return -EINVAL;
    ret = infilfs_extent_layout_validate(sb, object, &page_count);
    if (ret)
        return ret;
    if (page_index >= page_count)
        return -EFSCORRUPTED;

    if (version == INFILFS_OBJECT_VERSION_PAGED) {
        *block_out = le64_to_cpu(head_pointers[page_index]);
        return 0;
    }

    if (version == INFILFS_OBJECT_VERSION_TREE) {
        u32 levels = le32_to_cpu(head->reserved);
        u64 node = le64_to_cpu(head_pointers[0]);
        u64 index = page_index;
        u8 *page = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);

        if (!page)
            return -ENOMEM;
        while (levels) {
            const __le64 *pointers;
            u32 level = levels - 1u;
            u32 count;
            u64 span = 1;
            u64 slot;
            u32 i;

            for (i = 0; i < level; ++i)
                span *= INFILFS_EXTENT_INDEX_POINTERS_PER_PAGE;
            ret = infilfs_read_allocated_block(sb, node, page);
            if (ret)
                break;
            if (!infilfs_extent_pointer_page_valid(
                    sb, page, header->object_id, level, &pointers, &count)) {
                ret = -EFSCORRUPTED;
                break;
            }
            slot = div64_u64(index, span);
            if (slot >= count) {
                ret = -EFSCORRUPTED;
                break;
            }
            node = le64_to_cpu(pointers[slot]);
            index %= span;
            if (!level) {
                *block_out = node;
                ret = 0;
                break;
            }
            --levels;
        }
        kfree(page);
        return ret;
    }
    return -EFSCORRUPTED;
}
