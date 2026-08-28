// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Native read-side support for the generation-aware object-index radix tree.
 *
 * One persistent object-ID byte selects each 256-way branch.  Leaves reuse the
 * existing checksummed index-entry metadata page.  This keeps lookup bounded
 * by tree depth rather than by total object count while preserving Format
 * generation and CRC validation on every metadata block.
 */

static u64 infilfs_index_tree_root(
    const struct infilfs_index_payload_disk *payload)
{
    __le64 root_le;

    memcpy(&root_le, payload + 1, sizeof(root_le));
    return le64_to_cpu(root_le);
}

static bool infilfs_index_tree_branch_valid(
    struct super_block *sb, const u8 block[INFILFS_DISK_BLOCK_SIZE],
    const u8 owner_id[16], const __le64 **children_out, u32 *count_out)
{
    const struct infilfs_metadata_page_disk *page =
        (const struct infilfs_metadata_page_disk *)block;
    const __le64 *children;
    u32 expected;
    u32 seen = 0;
    u32 i;

    if (!infilfs_metadata_page_valid(
            sb, block, infilfs_index_branch_page_magic, owner_id))
        return false;
    expected = le32_to_cpu(page->entry_count);
    if (expected > INFILFS_INDEX_TREE_FANOUT ||
        le32_to_cpu(page->bytes_used) != INFILFS_INDEX_TREE_BRANCH_BYTES)
        return false;

    children = (const __le64 *)(page + 1);
    for (i = 0; i < INFILFS_INDEX_TREE_FANOUT; ++i) {
        u64 child = le64_to_cpu(children[i]);

        if (!child)
            continue;
        if (!infilfs_block_allocated(sb, child))
            return false;
        seen++;
    }
    if (seen != expected)
        return false;
    if (children_out)
        *children_out = children;
    if (count_out)
        *count_out = expected;
    return true;
}

static int infilfs_index_tree_lookup_node(
    struct super_block *sb, const u8 owner_id[16], u64 node,
    const u8 object_id[16], unsigned int depth,
    u64 *object_block_out, u16 *type_out)
{
    u8 *block;
    int ret;

    if (!node || depth > 16u || !infilfs_block_allocated(sb, node))
        return -EFSCORRUPTED;
    block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!block)
        return -ENOMEM;

    for (;;) {
        const struct infilfs_metadata_page_disk *page;
        const struct infilfs_index_entry_disk *entries;
        const __le64 *children;
        u32 count;
        u32 i;

        ret = infilfs_read_allocated_block(sb, node, block);
        if (ret)
            break;

        if (memcmp(block, infilfs_index_page_magic, 8) == 0) {
            if (!infilfs_metadata_page_valid(
                    sb, block, infilfs_index_page_magic, owner_id)) {
                ret = -EFSCORRUPTED;
                break;
            }
            page = (const struct infilfs_metadata_page_disk *)block;
            count = le32_to_cpu(page->entry_count);
            if (count > INFILFS_INDEX_ENTRIES_PER_PAGE ||
                le32_to_cpu(page->bytes_used) !=
                    count * sizeof(struct infilfs_index_entry_disk)) {
                ret = -EFSCORRUPTED;
                break;
            }
            entries = (const struct infilfs_index_entry_disk *)(page + 1);
            ret = -ENOENT;
            for (i = 0; i < count; ++i) {
                u64 object_block;

                if (memcmp(entries[i].object_id, object_id, 16) != 0)
                    continue;
                object_block = le64_to_cpu(entries[i].object_block);
                if (!infilfs_block_allocated(sb, object_block)) {
                    ret = -EFSCORRUPTED;
                    break;
                }
                *object_block_out = object_block;
                *type_out = le16_to_cpu(entries[i].object_type);
                ret = 0;
                break;
            }
            break;
        }

        if (depth >= 16u ||
            !infilfs_index_tree_branch_valid(
                sb, block, owner_id, &children, NULL)) {
            ret = -EFSCORRUPTED;
            break;
        }
        node = le64_to_cpu(children[object_id[depth++]]);
        if (!node) {
            ret = -ENOENT;
            break;
        }
        if (!infilfs_block_allocated(sb, node)) {
            ret = -EFSCORRUPTED;
            break;
        }
    }

    kfree(block);
    return ret;
}

static int infilfs_index_tree_lookup_head(
    struct super_block *sb, const u8 head[INFILFS_DISK_BLOCK_SIZE],
    const u8 object_id[16], u64 *object_block_out, u16 *type_out)
{
    const struct infilfs_object_header_disk *header =
        (const struct infilfs_object_header_disk *)head;
    const struct infilfs_index_payload_disk *payload =
        (const struct infilfs_index_payload_disk *)(header + 1);
    u64 root;

    if (le16_to_cpu(header->object_version) != INFILFS_OBJECT_VERSION_TREE ||
        le32_to_cpu(payload->reserved) != 0 ||
        !le32_to_cpu(payload->entry_count) ||
        le32_to_cpu(header->payload_size) !=
            sizeof(*payload) + sizeof(__le64))
        return -EFSCORRUPTED;
    root = infilfs_index_tree_root(payload);
    if (!root)
        return -EFSCORRUPTED;
    return infilfs_index_tree_lookup_node(
        sb, header->object_id, root, object_id, 0u,
        object_block_out, type_out);
}

static int infilfs_index_tree_snapshot_node(
    struct super_block *sb, const u8 owner_id[16], u64 node,
    unsigned int depth, u8 prefix[16],
    struct infilfs_index_entry_disk *out, u32 capacity, u32 *copied)
{
    u8 *block;
    int ret;

    if (!node || depth > 16u || !infilfs_block_allocated(sb, node))
        return -EFSCORRUPTED;
    block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!block)
        return -ENOMEM;
    ret = infilfs_read_allocated_block(sb, node, block);
    if (ret)
        goto out;

    if (memcmp(block, infilfs_index_page_magic, 8) == 0) {
        const struct infilfs_metadata_page_disk *page;
        const struct infilfs_index_entry_disk *entries;
        u32 count;
        u32 i;
        u32 j;

        if (!infilfs_metadata_page_valid(
                sb, block, infilfs_index_page_magic, owner_id)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        page = (const struct infilfs_metadata_page_disk *)block;
        count = le32_to_cpu(page->entry_count);
        if (count > INFILFS_INDEX_ENTRIES_PER_PAGE ||
            le32_to_cpu(page->bytes_used) !=
                count * sizeof(struct infilfs_index_entry_disk) ||
            *copied > capacity || count > capacity - *copied) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        entries = (const struct infilfs_index_entry_disk *)(page + 1);
        for (i = 0; i < count; ++i) {
            u16 type = le16_to_cpu(entries[i].object_type);

            if (memcmp(entries[i].object_id, prefix, depth) != 0 ||
                !memchr_inv(entries[i].object_id, 0, 16) ||
                type < INFILFS_OBJECT_DIRECTORY ||
                type > INFILFS_OBJECT_SNAPSHOT_CATALOG ||
                type == INFILFS_OBJECT_INDEX ||
                le16_to_cpu(entries[i].flags) != 0 ||
                le32_to_cpu(entries[i].reserved) != 0 ||
                !infilfs_block_allocated(
                    sb, le64_to_cpu(entries[i].object_block))) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            for (j = i + 1u; j < count; ++j)
                if (memcmp(entries[i].object_id,
                           entries[j].object_id, 16) == 0) {
                    ret = -EFSCORRUPTED;
                    goto out;
                }
        }
        memcpy(out + *copied, entries, (size_t)count * sizeof(*entries));
        *copied += count;
        ret = 0;
        goto out;
    }

    if (depth >= 16u) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    {
        const __le64 *children;
        u32 slot;

        if (!infilfs_index_tree_branch_valid(
                sb, block, owner_id, &children, NULL)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        for (slot = 0; slot < INFILFS_INDEX_TREE_FANOUT; ++slot) {
            u64 child = le64_to_cpu(children[slot]);

            if (!child)
                continue;
            prefix[depth] = (u8)slot;
            ret = infilfs_index_tree_snapshot_node(
                sb, owner_id, child, depth + 1u, prefix,
                out, capacity, copied);
            if (ret)
                goto out;
        }
    }
    ret = 0;
out:
    kfree(block);
    return ret;
}

static int infilfs_index_tree_snapshot(
    struct super_block *sb, const u8 head[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_index_entry_disk **entries_out, u32 *count_out)
{
    const struct infilfs_object_header_disk *header =
        (const struct infilfs_object_header_disk *)head;
    const struct infilfs_index_payload_disk *payload =
        (const struct infilfs_index_payload_disk *)(header + 1);
    struct infilfs_index_entry_disk *entries;
    u8 prefix[16] = { 0 };
    u32 count;
    u32 copied = 0;
    int ret;

    if (!entries_out || !count_out ||
        le16_to_cpu(header->object_version) != INFILFS_OBJECT_VERSION_TREE ||
        le32_to_cpu(payload->reserved) != 0 ||
        le32_to_cpu(header->payload_size) !=
            sizeof(*payload) + sizeof(__le64))
        return -EFSCORRUPTED;
    count = le32_to_cpu(payload->entry_count);
    if (!count || count > INFILFS_SB(sb)->device_blocks ||
        !infilfs_index_tree_root(payload))
        return -EFSCORRUPTED;

    entries = kvmalloc_array(count, sizeof(*entries), GFP_NOFS);
    if (!entries)
        return -ENOMEM;
    ret = infilfs_index_tree_snapshot_node(
        sb, header->object_id, infilfs_index_tree_root(payload), 0u,
        prefix, entries, count, &copied);
    if (ret || copied != count) {
        kvfree(entries);
        return ret ? ret : -EFSCORRUPTED;
    }
    *entries_out = entries;
    *count_out = count;
    return 0;
}
