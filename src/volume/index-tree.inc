// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Scalable generation-aware object-index radix tree.
 *
 * Object IDs are already uniformly distributed persistent 128-bit values.
 * Branch pages therefore use one object-ID byte per level and 256 physical
 * child pointers.  Leaves retain the existing validated index-entry layout.
 * Every modified node is copy-on-written through metadata_page_store(), so
 * snapshots and older checkpoints retain their exact historical tree roots.
 */

static uint64_t tree_index_root(
    const struct infs_index_payload_disk *payload)
{
    uint64_t root_le = 0;
    memcpy(&root_le, payload + 1, sizeof(root_le));
    return infs_le64_to_cpu(root_le);
}

static void tree_index_set_root(
    struct infs_index_payload_disk *payload, uint64_t root)
{
    uint64_t root_le = infs_cpu_to_le64(root);
    memcpy(payload + 1, &root_le, sizeof(root_le));
}

static int tree_branch_validate(
    struct infs_volume *vol, const uint8_t block[INFS_BLOCK_SIZE],
    const uint8_t owner_id[16], uint64_t **children_out,
    uint32_t *count_out)
{
    if (!infs_validate_metadata_page(
            block, INFS_INDEX_BRANCH_PAGE_MAGIC, owner_id) ||
        !metadata_page_generation_valid(vol, block))
        return INFS_STATUS_CORRUPT;

    const struct infs_metadata_page_disk *page =
        (const struct infs_metadata_page_disk *)block;
    uint32_t count = infs_le32_to_cpu(page->entry_count);
    if (count > INFS_INDEX_TREE_FANOUT ||
        infs_le32_to_cpu(page->bytes_used) != INFS_INDEX_TREE_BRANCH_BYTES)
        return INFS_STATUS_CORRUPT;

    uint64_t *children = (uint64_t *)(
        (uint8_t *)block + sizeof(struct infs_metadata_page_disk));
    uint32_t seen = 0;
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    for (uint32_t i = 0; i < INFS_INDEX_TREE_FANOUT; ++i) {
        uint64_t child = infs_le64_to_cpu(children[i]);
        if (!child)
            continue;
        ++seen;
        if (child >= total || !bitmap_get(vol->bitmap, child))
            return INFS_STATUS_CORRUPT;
    }
    if (seen != count)
        return INFS_STATUS_CORRUPT;
    if (children_out)
        *children_out = children;
    if (count_out)
        *count_out = count;
    return INFS_STATUS_OK;
}

static int tree_index_head_load(
    struct infs_volume *vol, uint8_t head[INFS_BLOCK_SIZE],
    struct infs_object_header_disk **header_out,
    struct infs_index_payload_disk **payload_out)
{
    struct infs_index_payload_disk *payload = NULL;
    infs_status status = index_load(vol, head, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)head;
    if (infs_le16_to_cpu(header->object_version) !=
            INFS_OBJECT_VERSION_TREE ||
        infs_le32_to_cpu(payload->reserved) != 0 ||
        infs_le32_to_cpu(payload->entry_count) == 0 ||
        infs_le32_to_cpu(header->payload_size) !=
            sizeof(*payload) + sizeof(uint64_t) ||
        tree_index_root(payload) == 0)
        return INFS_STATUS_CORRUPT;
    if (header_out)
        *header_out = header;
    if (payload_out)
        *payload_out = payload;
    return INFS_STATUS_OK;
}

static int tree_node_find(
    struct infs_volume *vol, const uint8_t owner_id[16], uint64_t node,
    const uint8_t id[16], unsigned depth, struct infs_lookup *out)
{
    if (!node || depth > 16u)
        return INFS_STATUS_CORRUPT;
    uint8_t block[INFS_BLOCK_SIZE];
    infs_status status = read_block(vol, node, block);
    if (status != INFS_STATUS_OK)
        return status;

    if (memcmp(block, INFS_INDEX_PAGE_MAGIC, 8) == 0) {
        struct infs_index_entry_disk *entries = NULL;
        uint32_t count = 0;
        status = index_page_validate(vol, block, owner_id, &entries, &count);
        if (status != INFS_STATUS_OK)
            return status;
        for (uint32_t i = 0; i < count; ++i) {
            if (memcmp(entries[i].object_id, id, 16) != 0)
                continue;
            uint64_t object_block = infs_le64_to_cpu(entries[i].object_block);
            if (object_block >= infs_le64_to_cpu(vol->sb.total_blocks) ||
                !bitmap_get(vol->bitmap, object_block))
                return INFS_STATUS_CORRUPT;
            if (out) {
                memcpy(out->object_id, id, 16);
                out->block = object_block;
                out->type = infs_le16_to_cpu(entries[i].object_type);
            }
            return INFS_STATUS_OK;
        }
        return INFS_STATUS_NOT_FOUND;
    }

    if (depth >= 16u)
        return INFS_STATUS_CORRUPT;
    uint64_t *children = NULL;
    status = tree_branch_validate(vol, block, owner_id, &children, NULL);
    if (status != INFS_STATUS_OK)
        return status;
    uint64_t child = infs_le64_to_cpu(children[id[depth]]);
    if (!child)
        return INFS_STATUS_NOT_FOUND;
    return tree_node_find(vol, owner_id, child, id, depth + 1u, out);
}

static int tree_build_subtree(
    struct infs_volume *vol, const uint8_t owner_id[16],
    const struct infs_index_entry_disk *entries, uint32_t count,
    unsigned depth, uint64_t *root_out)
{
    if (!count || !root_out)
        return INFS_STATUS_INVALID_ARGUMENT;

    uint8_t block[INFS_BLOCK_SIZE];
    infs_status status;
    if (count <= INFS_INDEX_ENTRIES_PER_PAGE) {
        status = infs_metadata_page_init(
            block, INFS_INDEX_PAGE_MAGIC, owner_id,
            infs_le64_to_cpu(vol->tx_base_sb.generation) + 1u);
        if (status != INFS_STATUS_OK)
            return status;
        struct infs_metadata_page_disk *page =
            (struct infs_metadata_page_disk *)block;
        page->entry_count = infs_cpu_to_le32(count);
        page->bytes_used = infs_cpu_to_le32(
            count * sizeof(struct infs_index_entry_disk));
        memcpy(page + 1, entries, (size_t)count * sizeof(*entries));
        return metadata_page_store(vol, 0, block, root_out);
    }
    if (depth >= 16u)
        return INFS_STATUS_CORRUPT;

    status = infs_metadata_page_init(
        block, INFS_INDEX_BRANCH_PAGE_MAGIC, owner_id,
        infs_le64_to_cpu(vol->tx_base_sb.generation) + 1u);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_metadata_page_disk *page =
        (struct infs_metadata_page_disk *)block;
    uint64_t *children = (uint64_t *)(page + 1);
    struct infs_index_entry_disk *group = malloc(
        (size_t)count * sizeof(*group));
    if (!group)
        return INFS_STATUS_NO_MEMORY;

    uint32_t child_count = 0;
    for (uint32_t slot = 0; slot < INFS_INDEX_TREE_FANOUT; ++slot) {
        uint32_t n = 0;
        for (uint32_t i = 0; i < count; ++i)
            if (entries[i].object_id[depth] == (uint8_t)slot)
                group[n++] = entries[i];
        if (!n)
            continue;
        uint64_t child = 0;
        status = tree_build_subtree(
            vol, owner_id, group, n, depth + 1u, &child);
        if (status != INFS_STATUS_OK) {
            free(group);
            return status;
        }
        children[slot] = infs_cpu_to_le64(child);
        ++child_count;
    }
    free(group);
    page->entry_count = infs_cpu_to_le32(child_count);
    page->bytes_used = infs_cpu_to_le32(INFS_INDEX_TREE_BRANCH_BYTES);
    return metadata_page_store(vol, 0, block, root_out);
}

enum tree_index_mutation {
    TREE_INDEX_REPOINT,
    TREE_INDEX_ADD,
    TREE_INDEX_REMOVE
};

static int tree_node_mutate(
    struct infs_volume *vol, const uint8_t owner_id[16], uint64_t node,
    const uint8_t id[16], unsigned depth, enum tree_index_mutation mutation,
    uint64_t object_block, uint16_t type, uint64_t *replacement_out)
{
    uint8_t block[INFS_BLOCK_SIZE];
    infs_status status = read_block(vol, node, block);
    if (status != INFS_STATUS_OK)
        return status;

    if (memcmp(block, INFS_INDEX_PAGE_MAGIC, 8) == 0) {
        struct infs_index_entry_disk *entries = NULL;
        uint32_t count = 0;
        status = index_page_validate(vol, block, owner_id, &entries, &count);
        if (status != INFS_STATUS_OK)
            return status;
        uint32_t found = count;
        for (uint32_t i = 0; i < count; ++i)
            if (memcmp(entries[i].object_id, id, 16) == 0) {
                found = i;
                break;
            }

        if (mutation == TREE_INDEX_ADD) {
            if (found != count)
                return INFS_STATUS_ALREADY_EXISTS;
            if (count < INFS_INDEX_ENTRIES_PER_PAGE) {
                struct infs_index_entry_disk *entry = &entries[count];
                memset(entry, 0, sizeof(*entry));
                memcpy(entry->object_id, id, 16);
                entry->object_block = infs_cpu_to_le64(object_block);
                entry->object_type = infs_cpu_to_le16(type);
                struct infs_metadata_page_disk *page =
                    (struct infs_metadata_page_disk *)block;
                page->entry_count = infs_cpu_to_le32(count + 1u);
                page->bytes_used = infs_cpu_to_le32(
                    (count + 1u) * sizeof(*entry));
                return metadata_page_store(
                    vol, node, block, replacement_out);
            }

            struct infs_index_entry_disk *grown = malloc(
                (size_t)(count + 1u) * sizeof(*grown));
            if (!grown)
                return INFS_STATUS_NO_MEMORY;
            memcpy(grown, entries, (size_t)count * sizeof(*grown));
            memset(&grown[count], 0, sizeof(grown[count]));
            memcpy(grown[count].object_id, id, 16);
            grown[count].object_block = infs_cpu_to_le64(object_block);
            grown[count].object_type = infs_cpu_to_le16(type);
            status = tree_build_subtree(
                vol, owner_id, grown, count + 1u, depth, replacement_out);
            free(grown);
            if (status != INFS_STATUS_OK)
                return status;
            free_run(vol, node, 1);
            return vol->tx_error;
        }

        if (found == count)
            return INFS_STATUS_NOT_FOUND;
        if (mutation == TREE_INDEX_REPOINT) {
            entries[found].object_block = infs_cpu_to_le64(object_block);
            entries[found].object_type = infs_cpu_to_le16(type);
            return metadata_page_store(vol, node, block, replacement_out);
        }

        if (count == 1u) {
            free_run(vol, node, 1);
            if (vol->tx_error != INFS_STATUS_OK)
                return vol->tx_error;
            *replacement_out = 0;
            return INFS_STATUS_OK;
        }
        if (found + 1u < count)
            memmove(&entries[found], &entries[found + 1u],
                    (size_t)(count - found - 1u) * sizeof(*entries));
        memset(&entries[count - 1u], 0, sizeof(*entries));
        struct infs_metadata_page_disk *page =
            (struct infs_metadata_page_disk *)block;
        page->entry_count = infs_cpu_to_le32(count - 1u);
        page->bytes_used = infs_cpu_to_le32(
            (count - 1u) * sizeof(*entries));
        return metadata_page_store(vol, node, block, replacement_out);
    }

    if (depth >= 16u)
        return INFS_STATUS_CORRUPT;
    uint64_t *children = NULL;
    uint32_t child_count = 0;
    status = tree_branch_validate(
        vol, block, owner_id, &children, &child_count);
    if (status != INFS_STATUS_OK)
        return status;

    uint32_t slot = id[depth];
    uint64_t child = infs_le64_to_cpu(children[slot]);
    uint64_t replacement = 0;
    if (!child) {
        if (mutation != TREE_INDEX_ADD)
            return INFS_STATUS_NOT_FOUND;
        struct infs_index_entry_disk entry;
        memset(&entry, 0, sizeof(entry));
        memcpy(entry.object_id, id, 16);
        entry.object_block = infs_cpu_to_le64(object_block);
        entry.object_type = infs_cpu_to_le16(type);
        status = tree_build_subtree(
            vol, owner_id, &entry, 1u, depth + 1u, &replacement);
        if (status != INFS_STATUS_OK)
            return status;
        ++child_count;
    } else {
        status = tree_node_mutate(
            vol, owner_id, child, id, depth + 1u, mutation,
            object_block, type, &replacement);
        if (status != INFS_STATUS_OK)
            return status;
        if (!replacement)
            --child_count;
    }

    if (!child_count) {
        free_run(vol, node, 1);
        if (vol->tx_error != INFS_STATUS_OK)
            return vol->tx_error;
        *replacement_out = 0;
        return INFS_STATUS_OK;
    }
    children[slot] = infs_cpu_to_le64(replacement);
    struct infs_metadata_page_disk *page =
        (struct infs_metadata_page_disk *)block;
    page->entry_count = infs_cpu_to_le32(child_count);
    return metadata_page_store(vol, node, block, replacement_out);
}

static int tree_index_apply(
    struct infs_volume *vol, const uint8_t id[16],
    enum tree_index_mutation mutation, uint64_t object_block, uint16_t type)
{
    uint8_t head[INFS_BLOCK_SIZE];
    struct infs_object_header_disk *header = NULL;
    struct infs_index_payload_disk *payload = NULL;
    infs_status status = tree_index_head_load(vol, head, &header, &payload);
    if (status != INFS_STATUS_OK)
        return status;

    uint32_t count = infs_le32_to_cpu(payload->entry_count);
    uint64_t replacement = 0;
    status = tree_node_mutate(
        vol, header->object_id, tree_index_root(payload), id, 0u, mutation,
        object_block, type, &replacement);
    if (status != INFS_STATUS_OK)
        return status;

    if (mutation == TREE_INDEX_ADD) {
        if (count == UINT32_MAX)
            return INFS_STATUS_OVERFLOW;
        ++count;
    } else if (mutation == TREE_INDEX_REMOVE) {
        if (count <= 1u || replacement == 0)
            return INFS_STATUS_CORRUPT;
        --count;
    }
    payload->entry_count = infs_cpu_to_le32(count);
    tree_index_set_root(payload, replacement);
    header->payload_size = infs_cpu_to_le32(
        sizeof(*payload) + sizeof(uint64_t));
    return write_object(
        vol, infs_le64_to_cpu(vol->sb.object_index_block), head);
}

static int tree_index_find(struct infs_volume *vol, const uint8_t id[16],
                           struct infs_lookup *out)
{
    uint8_t head[INFS_BLOCK_SIZE];
    struct infs_object_header_disk *header = NULL;
    struct infs_index_payload_disk *payload = NULL;
    infs_status status = tree_index_head_load(vol, head, &header, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    return tree_node_find(
        vol, header->object_id, tree_index_root(payload), id, 0u, out);
}

static int tree_index_repoint(struct infs_volume *vol, const uint8_t id[16],
                              uint64_t object_block, uint16_t type)
{
    return tree_index_apply(
        vol, id, TREE_INDEX_REPOINT, object_block, type);
}

static int tree_index_add(struct infs_volume *vol, const uint8_t id[16],
                          uint64_t object_block, uint16_t type)
{
    return tree_index_apply(vol, id, TREE_INDEX_ADD, object_block, type);
}

static int tree_index_remove(struct infs_volume *vol, const uint8_t id[16])
{
    return tree_index_apply(vol, id, TREE_INDEX_REMOVE, 0, 0);
}

static int tree_snapshot_node(
    struct infs_volume *vol, const uint8_t owner_id[16], uint64_t node,
    unsigned depth, uint8_t prefix[16],
    struct infs_index_entry_disk *out, uint32_t capacity, uint32_t *copied)
{
    if (!node || depth > 16u)
        return INFS_STATUS_CORRUPT;
    uint8_t block[INFS_BLOCK_SIZE];
    infs_status status = read_block(vol, node, block);
    if (status != INFS_STATUS_OK)
        return status;

    if (memcmp(block, INFS_INDEX_PAGE_MAGIC, 8) == 0) {
        struct infs_index_entry_disk *entries = NULL;
        uint32_t count = 0;
        status = index_page_validate(vol, block, owner_id, &entries, &count);
        if (status != INFS_STATUS_OK || count > capacity - *copied)
            return INFS_STATUS_CORRUPT;
        for (uint32_t i = 0; i < count; ++i) {
            if (memcmp(entries[i].object_id, prefix, depth) != 0)
                return INFS_STATUS_CORRUPT;
            uint64_t object_block = infs_le64_to_cpu(entries[i].object_block);
            if (object_block >= infs_le64_to_cpu(vol->sb.total_blocks) ||
                !bitmap_get(vol->bitmap, object_block))
                return INFS_STATUS_CORRUPT;
            for (uint32_t j = i + 1u; j < count; ++j)
                if (memcmp(entries[i].object_id, entries[j].object_id, 16) == 0)
                    return INFS_STATUS_CORRUPT;
        }
        memcpy(out + *copied, entries, (size_t)count * sizeof(*entries));
        *copied += count;
        return INFS_STATUS_OK;
    }

    if (depth >= 16u)
        return INFS_STATUS_CORRUPT;
    uint64_t *children = NULL;
    status = tree_branch_validate(vol, block, owner_id, &children, NULL);
    if (status != INFS_STATUS_OK)
        return status;
    for (uint32_t slot = 0; slot < INFS_INDEX_TREE_FANOUT; ++slot) {
        uint64_t child = infs_le64_to_cpu(children[slot]);
        if (!child)
            continue;
        prefix[depth] = (uint8_t)slot;
        status = tree_snapshot_node(
            vol, owner_id, child, depth + 1u, prefix,
            out, capacity, copied);
        if (status != INFS_STATUS_OK)
            return status;
    }
    return INFS_STATUS_OK;
}

static int tree_index_snapshot(struct infs_volume *vol,
                               struct infs_index_entry_disk **entries_out,
                               uint32_t *count_out)
{
    if (!entries_out || !count_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    *entries_out = NULL;
    *count_out = 0;

    uint8_t head[INFS_BLOCK_SIZE];
    struct infs_object_header_disk *header = NULL;
    struct infs_index_payload_disk *payload = NULL;
    infs_status status = tree_index_head_load(vol, head, &header, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    uint32_t count = infs_le32_to_cpu(payload->entry_count);
    struct infs_index_entry_disk *entries = calloc(
        count ? count : 1u, sizeof(*entries));
    if (!entries)
        return INFS_STATUS_NO_MEMORY;
    uint8_t prefix[16] = {0};
    uint32_t copied = 0;
    status = tree_snapshot_node(
        vol, header->object_id, tree_index_root(payload), 0u, prefix,
        entries, count, &copied);
    if (status != INFS_STATUS_OK || copied != count) {
        free(entries);
        return status == INFS_STATUS_OK ? INFS_STATUS_CORRUPT : status;
    }
    *entries_out = entries;
    *count_out = count;
    return INFS_STATUS_OK;
}
