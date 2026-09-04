// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Scalable directory radix tree.
 *
 * Names are routed by the complete SHA-256 digest, one byte per branch level.
 * Leaves retain the ordinary validated variable-length dirent representation.
 * Exact name bytes are always compared at the leaf, so the hash is only a
 * routing accelerator and never the namespace identity. A full leaf whose
 * complete 256-bit digest cannot be separated fails closed rather than
 * silently aliasing names.
 */

static uint64_t tree_dir_root(const struct infs_directory_payload_disk *payload)
{
    uint64_t root_le = 0;
    memcpy(&root_le, payload + 1, sizeof(root_le));
    return infs_le64_to_cpu(root_le);
}

static void tree_dir_set_root(struct infs_directory_payload_disk *payload,
                              uint64_t root)
{
    uint64_t root_le = infs_cpu_to_le64(root);
    memcpy(payload + 1, &root_le, sizeof(root_le));
}

static void tree_dir_digest(const char *name, size_t len, uint8_t digest[32])
{
    infs_sha256(name, len, digest);
}

static int tree_dir_branch_validate(
    struct infs_volume *vol, const uint8_t block[INFS_BLOCK_SIZE],
    const uint8_t owner_id[16], uint64_t **children_out, uint32_t *count_out)
{
    if (!infs_validate_metadata_page(
            block, INFS_DIRECTORY_BRANCH_PAGE_MAGIC, owner_id) ||
        !metadata_page_generation_valid(vol, block))
        return INFS_STATUS_CORRUPT;
    const struct infs_metadata_page_disk *page =
        (const struct infs_metadata_page_disk *)block;
    uint32_t count = infs_le32_to_cpu(page->entry_count);
    if (count > INFS_DIRECTORY_TREE_FANOUT ||
        infs_le32_to_cpu(page->bytes_used) != INFS_DIRECTORY_TREE_BRANCH_BYTES)
        return INFS_STATUS_CORRUPT;

    uint64_t *children = (uint64_t *)(page + 1);
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    uint32_t seen = 0;
    for (uint32_t i = 0; i < INFS_DIRECTORY_TREE_FANOUT; ++i) {
        uint64_t child = infs_le64_to_cpu(children[i]);
        if (!child)
            continue;
        if (child >= total || !bitmap_get(vol->bitmap, child))
            return INFS_STATUS_CORRUPT;
        ++seen;
    }
    if (seen != count)
        return INFS_STATUS_CORRUPT;
    if (children_out)
        *children_out = children;
    if (count_out)
        *count_out = count;
    return INFS_STATUS_OK;
}

static int tree_dir_leaf_find(
    struct infs_volume *vol, const uint8_t block[INFS_BLOCK_SIZE],
    const uint8_t owner_id[16], const char *name,
    struct infs_dirent_disk *entry_out)
{
    uint8_t *entries = NULL;
    uint32_t bytes = 0;
    int status = directory_page_validate(
        vol, block, owner_id, &entries, &bytes, NULL);
    if (status != INFS_STATUS_OK)
        return status;
    size_t wanted = strlen(name);
    uint32_t offset = 0;
    while (offset < bytes) {
        struct infs_dirent_disk *entry =
            (struct infs_dirent_disk *)(entries + offset);
        uint16_t rec = infs_le16_to_cpu(entry->record_size);
        uint16_t len = infs_le16_to_cpu(entry->name_length);
        if (len == wanted &&
            memcmp(entries + offset + sizeof(*entry), name, wanted) == 0) {
            if (entry_out)
                *entry_out = *entry;
            return INFS_STATUS_OK;
        }
        offset += rec;
    }
    return INFS_STATUS_NOT_FOUND;
}

static int tree_dir_lookup(struct infs_volume *vol,
                           uint8_t head[INFS_BLOCK_SIZE], const char *name,
                           struct infs_dirent_disk *entry_out)
{
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)head;
    struct infs_directory_payload_disk *payload =
        (struct infs_directory_payload_disk *)(header + 1);
    if (directory_cache_lookup(vol, header->object_id, name, entry_out))
        return INFS_STATUS_OK;
    if (directory_cache_complete(vol, header->object_id))
        return INFS_STATUS_NOT_FOUND;

    uint64_t node = tree_dir_root(payload);
    if (!node)
        return infs_le32_to_cpu(payload->entry_count) == 0 ?
            INFS_STATUS_NOT_FOUND : INFS_STATUS_CORRUPT;

    uint8_t digest[32];
    tree_dir_digest(name, strlen(name), digest);
    for (unsigned depth = 0; depth <= INFS_DIRECTORY_TREE_DEPTH; ++depth) {
        uint8_t block[INFS_BLOCK_SIZE];
        int status = read_block(vol, node, block);
        if (status != INFS_STATUS_OK)
            return status;
        if (memcmp(block, INFS_DIRECTORY_PAGE_MAGIC, 8) == 0) {
            status = tree_dir_leaf_find(
                vol, block, header->object_id, name, entry_out);
            if (status == INFS_STATUS_OK && entry_out)
                (void)directory_cache_store(
                    vol, header->object_id, name, entry_out->object_id,
                    infs_le16_to_cpu(entry_out->object_type),
                    INFS_DIRECTORY_PAGE_UNKNOWN);
            return status;
        }
        if (depth >= INFS_DIRECTORY_TREE_DEPTH)
            return INFS_STATUS_CORRUPT;
        uint64_t *children = NULL;
        status = tree_dir_branch_validate(
            vol, block, header->object_id, &children, NULL);
        if (status != INFS_STATUS_OK)
            return status;
        node = infs_le64_to_cpu(children[digest[depth]]);
        if (!node)
            return INFS_STATUS_NOT_FOUND;
    }
    return INFS_STATUS_CORRUPT;
}

static int tree_dir_append_snapshot(
    uint8_t **buffer, size_t *length, size_t *capacity,
    const uint8_t *entries, uint32_t bytes)
{
    if (SIZE_MAX - *length < bytes)
        return INFS_STATUS_OVERFLOW;
    size_t needed = *length + bytes;
    if (needed > *capacity) {
        size_t next = *capacity ? *capacity : INFS_METADATA_PAGE_DATA_SIZE;
        while (next < needed) {
            if (next > SIZE_MAX / 2u) {
                next = needed;
                break;
            }
            next *= 2u;
        }
        uint8_t *grown = realloc(*buffer, next);
        if (!grown)
            return INFS_STATUS_NO_MEMORY;
        *buffer = grown;
        *capacity = next;
    }
    if (bytes)
        memcpy(*buffer + *length, entries, bytes);
    *length = needed;
    return INFS_STATUS_OK;
}

static int tree_dir_snapshot_node(
    struct infs_volume *vol, const uint8_t owner_id[16], uint64_t node,
    unsigned depth, uint8_t **buffer, size_t *length, size_t *capacity,
    uint32_t *count)
{
    if (!node || depth > INFS_DIRECTORY_TREE_DEPTH)
        return INFS_STATUS_CORRUPT;
    uint8_t block[INFS_BLOCK_SIZE];
    int status = read_block(vol, node, block);
    if (status != INFS_STATUS_OK)
        return status;
    if (memcmp(block, INFS_DIRECTORY_PAGE_MAGIC, 8) == 0) {
        uint8_t *entries = NULL;
        uint32_t bytes = 0, leaf_count = 0;
        status = directory_page_validate(
            vol, block, owner_id, &entries, &bytes, &leaf_count);
        if (status != INFS_STATUS_OK || UINT32_MAX - *count < leaf_count)
            return status == INFS_STATUS_OK ? INFS_STATUS_OVERFLOW : status;
        status = tree_dir_append_snapshot(
            buffer, length, capacity, entries, bytes);
        if (status == INFS_STATUS_OK)
            *count += leaf_count;
        return status;
    }
    if (depth >= INFS_DIRECTORY_TREE_DEPTH)
        return INFS_STATUS_CORRUPT;
    uint64_t *children = NULL;
    status = tree_dir_branch_validate(vol, block, owner_id, &children, NULL);
    if (status != INFS_STATUS_OK)
        return status;
    for (uint32_t slot = 0; slot < INFS_DIRECTORY_TREE_FANOUT; ++slot) {
        uint64_t child = infs_le64_to_cpu(children[slot]);
        if (!child)
            continue;
        status = tree_dir_snapshot_node(
            vol, owner_id, child, depth + 1u,
            buffer, length, capacity, count);
        if (status != INFS_STATUS_OK)
            return status;
    }
    return INFS_STATUS_OK;
}

static int tree_dir_snapshot(struct infs_volume *vol,
                             uint8_t head[INFS_BLOCK_SIZE],
                             uint8_t **bytes_out, size_t *size_out,
                             uint32_t *count_out)
{
    if (!bytes_out || !size_out || !count_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    *bytes_out = NULL;
    *size_out = 0;
    *count_out = 0;
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)head;
    struct infs_directory_payload_disk *payload =
        (struct infs_directory_payload_disk *)(header + 1);
    uint32_t expected = infs_le32_to_cpu(payload->entry_count);
    uint64_t root = tree_dir_root(payload);
    if (!expected)
        return root == 0 ? INFS_STATUS_OK : INFS_STATUS_CORRUPT;
    if (!root)
        return INFS_STATUS_CORRUPT;
    uint8_t *buffer = NULL;
    size_t length = 0, capacity = 0;
    uint32_t count = 0;
    int status = tree_dir_snapshot_node(
        vol, header->object_id, root, 0u,
        &buffer, &length, &capacity, &count);
    if (status != INFS_STATUS_OK || count != expected) {
        free(buffer);
        return status == INFS_STATUS_OK ? INFS_STATUS_CORRUPT : status;
    }
    *bytes_out = buffer;
    *size_out = length;
    *count_out = count;
    return INFS_STATUS_OK;
}

static int tree_dir_build_subtree(
    struct infs_volume *vol, const uint8_t owner_id[16],
    const uint8_t *entries, size_t bytes, uint32_t count,
    unsigned depth, uint64_t *root_out)
{
    if (!entries || !bytes || !count || !root_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (bytes <= INFS_METADATA_PAGE_DATA_SIZE) {
        uint8_t leaf[INFS_BLOCK_SIZE];
        int status = infs_metadata_page_init(
            leaf, INFS_DIRECTORY_PAGE_MAGIC, owner_id,
            infs_le64_to_cpu(vol->tx_base_sb.generation) + 1u);
        if (status != INFS_STATUS_OK)
            return status;
        struct infs_metadata_page_disk *page =
            (struct infs_metadata_page_disk *)leaf;
        memcpy(page + 1, entries, bytes);
        page->entry_count = infs_cpu_to_le32(count);
        page->bytes_used = infs_cpu_to_le32((uint32_t)bytes);
        return metadata_page_store(vol, 0, leaf, root_out);
    }
    if (depth >= INFS_DIRECTORY_TREE_DEPTH)
        return INFS_STATUS_NO_SPACE;

    uint8_t branch[INFS_BLOCK_SIZE];
    int status = infs_metadata_page_init(
        branch, INFS_DIRECTORY_BRANCH_PAGE_MAGIC, owner_id,
        infs_le64_to_cpu(vol->tx_base_sb.generation) + 1u);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_metadata_page_disk *page =
        (struct infs_metadata_page_disk *)branch;
    uint64_t *children = (uint64_t *)(page + 1);
    uint8_t *group = malloc(bytes);
    if (!group)
        return INFS_STATUS_NO_MEMORY;

    uint32_t child_count = 0;
    for (uint32_t slot = 0; slot < INFS_DIRECTORY_TREE_FANOUT; ++slot) {
        size_t in_off = 0, group_bytes = 0;
        uint32_t group_count = 0;
        while (in_off < bytes) {
            if (bytes - in_off < sizeof(struct infs_dirent_disk)) {
                free(group);
                return INFS_STATUS_CORRUPT;
            }
            const struct infs_dirent_disk *entry =
                (const struct infs_dirent_disk *)(entries + in_off);
            uint16_t rec = infs_le16_to_cpu(entry->record_size);
            uint16_t len = infs_le16_to_cpu(entry->name_length);
            if (!rec || rec > bytes - in_off ||
                sizeof(*entry) + len > rec) {
                free(group);
                return INFS_STATUS_CORRUPT;
            }
            uint8_t digest[32];
            tree_dir_digest(
                (const char *)entries + in_off + sizeof(*entry), len, digest);
            if (digest[depth] == (uint8_t)slot) {
                memcpy(group + group_bytes, entries + in_off, rec);
                group_bytes += rec;
                group_count++;
            }
            in_off += rec;
        }
        if (!group_count)
            continue;
        uint64_t child = 0;
        status = tree_dir_build_subtree(
            vol, owner_id, group, group_bytes, group_count,
            depth + 1u, &child);
        if (status != INFS_STATUS_OK) {
            free(group);
            return status;
        }
        children[slot] = infs_cpu_to_le64(child);
        child_count++;
    }
    free(group);
    if (!child_count)
        return INFS_STATUS_CORRUPT;
    page->entry_count = infs_cpu_to_le32(child_count);
    page->bytes_used = infs_cpu_to_le32(INFS_DIRECTORY_TREE_BRANCH_BYTES);
    return metadata_page_store(vol, 0, branch, root_out);
}

enum tree_dir_mutation { TREE_DIR_ADD, TREE_DIR_REMOVE };

static int tree_dir_mutate_node(
    struct infs_volume *vol, const uint8_t owner_id[16], uint64_t node,
    const char *name, const uint8_t object_id[16], uint16_t type,
    const uint8_t digest[32], unsigned depth, enum tree_dir_mutation mutation,
    struct infs_dirent_disk *removed_copy, uint64_t *replacement_out)
{
    uint8_t block[INFS_BLOCK_SIZE];
    int status = read_block(vol, node, block);
    if (status != INFS_STATUS_OK)
        return status;
    if (memcmp(block, INFS_DIRECTORY_PAGE_MAGIC, 8) == 0) {
        uint8_t *entries = NULL;
        uint32_t bytes = 0, count = 0;
        status = directory_page_validate(
            vol, block, owner_id, &entries, &bytes, &count);
        if (status != INFS_STATUS_OK)
            return status;
        size_t wanted = strlen(name);
        uint32_t offset = 0, found_offset = UINT32_MAX;
        uint16_t found_rec = 0;
        while (offset < bytes) {
            struct infs_dirent_disk *entry =
                (struct infs_dirent_disk *)(entries + offset);
            uint16_t rec = infs_le16_to_cpu(entry->record_size);
            uint16_t len = infs_le16_to_cpu(entry->name_length);
            if (len == wanted &&
                memcmp(entries + offset + sizeof(*entry), name, wanted) == 0) {
                found_offset = offset;
                found_rec = rec;
                if (removed_copy)
                    *removed_copy = *entry;
                break;
            }
            offset += rec;
        }
        if (mutation == TREE_DIR_REMOVE) {
            if (found_offset == UINT32_MAX)
                return INFS_STATUS_NOT_FOUND;
            if (count == 1u) {
                free_run(vol, node, 1);
                if (vol->tx_error != INFS_STATUS_OK)
                    return vol->tx_error;
                *replacement_out = 0;
                return INFS_STATUS_OK;
            }
            memmove(entries + found_offset, entries + found_offset + found_rec,
                    bytes - found_offset - found_rec);
            memset(entries + bytes - found_rec, 0, found_rec);
            struct infs_metadata_page_disk *page =
                (struct infs_metadata_page_disk *)block;
            page->entry_count = infs_cpu_to_le32(count - 1u);
            page->bytes_used = infs_cpu_to_le32(bytes - found_rec);
            return metadata_page_store(vol, node, block, replacement_out);
        }
        if (found_offset != UINT32_MAX)
            return INFS_STATUS_ALREADY_EXISTS;
        size_t rec = align8(sizeof(struct infs_dirent_disk) + wanted);
        if (rec <= INFS_METADATA_PAGE_DATA_SIZE - bytes) {
            uint8_t *cursor = entries + bytes;
            memset(cursor, 0, rec);
            struct infs_dirent_disk *entry = (struct infs_dirent_disk *)cursor;
            entry->record_size = infs_cpu_to_le16((uint16_t)rec);
            entry->name_length = infs_cpu_to_le16((uint16_t)wanted);
            entry->object_type = infs_cpu_to_le16(type);
            memcpy(entry->object_id, object_id, 16);
            memcpy(cursor + sizeof(*entry), name, wanted);
            struct infs_metadata_page_disk *page =
                (struct infs_metadata_page_disk *)block;
            page->entry_count = infs_cpu_to_le32(count + 1u);
            page->bytes_used = infs_cpu_to_le32((uint32_t)(bytes + rec));
            return metadata_page_store(vol, node, block, replacement_out);
        }
        uint8_t *grown = malloc((size_t)bytes + rec);
        if (!grown)
            return INFS_STATUS_NO_MEMORY;
        memcpy(grown, entries, bytes);
        uint8_t *cursor = grown + bytes;
        memset(cursor, 0, rec);
        struct infs_dirent_disk *entry = (struct infs_dirent_disk *)cursor;
        entry->record_size = infs_cpu_to_le16((uint16_t)rec);
        entry->name_length = infs_cpu_to_le16((uint16_t)wanted);
        entry->object_type = infs_cpu_to_le16(type);
        memcpy(entry->object_id, object_id, 16);
        memcpy(cursor + sizeof(*entry), name, wanted);
        status = tree_dir_build_subtree(
            vol, owner_id, grown, (size_t)bytes + rec, count + 1u,
            depth, replacement_out);
        free(grown);
        if (status != INFS_STATUS_OK)
            return status;
        free_run(vol, node, 1);
        return vol->tx_error;
    }

    if (depth >= INFS_DIRECTORY_TREE_DEPTH)
        return INFS_STATUS_CORRUPT;
    uint64_t *children = NULL;
    uint32_t child_count = 0;
    status = tree_dir_branch_validate(
        vol, block, owner_id, &children, &child_count);
    if (status != INFS_STATUS_OK)
        return status;
    uint32_t slot = digest[depth];
    uint64_t child = infs_le64_to_cpu(children[slot]);
    uint64_t replacement = 0;
    if (!child) {
        if (mutation != TREE_DIR_ADD)
            return INFS_STATUS_NOT_FOUND;
        size_t name_len = strlen(name);
        size_t rec = align8(sizeof(struct infs_dirent_disk) + name_len);
        uint8_t *single = calloc(1, rec);
        if (!single)
            return INFS_STATUS_NO_MEMORY;
        struct infs_dirent_disk *entry = (struct infs_dirent_disk *)single;
        entry->record_size = infs_cpu_to_le16((uint16_t)rec);
        entry->name_length = infs_cpu_to_le16((uint16_t)name_len);
        entry->object_type = infs_cpu_to_le16(type);
        memcpy(entry->object_id, object_id, 16);
        memcpy(single + sizeof(*entry), name, name_len);
        status = tree_dir_build_subtree(
            vol, owner_id, single, rec, 1u, depth + 1u, &replacement);
        free(single);
        if (status != INFS_STATUS_OK)
            return status;
        child_count++;
    } else {
        status = tree_dir_mutate_node(
            vol, owner_id, child, name, object_id, type, digest,
            depth + 1u, mutation, removed_copy, &replacement);
        if (status != INFS_STATUS_OK)
            return status;
        if (!replacement)
            child_count--;
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

static int tree_dir_add(struct infs_volume *vol,
                        uint8_t head[INFS_BLOCK_SIZE], const char *name,
                        const uint8_t object_id[16], uint16_t type)
{
    size_t len = strlen(name);
    if (!len || len > INFS_NAME_MAX || strchr(name, '/') ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
        id_is_zero(object_id) || !infs_utf8_validate(name, len) ||
        !namespace_object_type(type))
        return INFS_STATUS_INVALID_ARGUMENT;
    int status = tree_dir_lookup(vol, head, name, NULL);
    if (status == INFS_STATUS_OK)
        return INFS_STATUS_ALREADY_EXISTS;
    if (status != INFS_STATUS_NOT_FOUND)
        return status;

    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)head;
    struct infs_directory_payload_disk *payload =
        (struct infs_directory_payload_disk *)(header + 1);
    uint8_t digest[32];
    tree_dir_digest(name, len, digest);
    uint64_t root = tree_dir_root(payload), replacement = 0;
    if (!root) {
        size_t rec = align8(sizeof(struct infs_dirent_disk) + len);
        uint8_t *single = calloc(1, rec);
        if (!single)
            return INFS_STATUS_NO_MEMORY;
        struct infs_dirent_disk *entry = (struct infs_dirent_disk *)single;
        entry->record_size = infs_cpu_to_le16((uint16_t)rec);
        entry->name_length = infs_cpu_to_le16((uint16_t)len);
        entry->object_type = infs_cpu_to_le16(type);
        memcpy(entry->object_id, object_id, 16);
        memcpy(single + sizeof(*entry), name, len);
        status = tree_dir_build_subtree(
            vol, header->object_id, single, rec, 1u, 0u, &replacement);
        free(single);
    } else {
        status = tree_dir_mutate_node(
            vol, header->object_id, root, name, object_id, type,
            digest, 0u, TREE_DIR_ADD, NULL, &replacement);
    }
    if (status != INFS_STATUS_OK)
        return status;
    uint32_t count = infs_le32_to_cpu(payload->entry_count);
    if (count == UINT32_MAX)
        return INFS_STATUS_OVERFLOW;
    payload->entry_count = infs_cpu_to_le32(count + 1u);
    payload->bytes_used = 0;
    tree_dir_set_root(payload, replacement);
    header->payload_size = infs_cpu_to_le32(
        (uint32_t)(sizeof(*payload) + sizeof(uint64_t)));
    (void)directory_cache_store(
        vol, header->object_id, name, object_id, type,
        INFS_DIRECTORY_PAGE_UNKNOWN);
    return INFS_STATUS_OK;
}

static int tree_dir_remove(struct infs_volume *vol,
                           uint8_t head[INFS_BLOCK_SIZE], const char *name,
                           struct infs_dirent_disk *removed_copy)
{
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)head;
    struct infs_directory_payload_disk *payload =
        (struct infs_directory_payload_disk *)(header + 1);
    uint32_t count = infs_le32_to_cpu(payload->entry_count);
    uint64_t root = tree_dir_root(payload);
    if (!count || !root)
        return count == 0 && root == 0 ?
            INFS_STATUS_NOT_FOUND : INFS_STATUS_CORRUPT;
    uint8_t digest[32];
    tree_dir_digest(name, strlen(name), digest);
    uint64_t replacement = 0;
    int status = tree_dir_mutate_node(
        vol, header->object_id, root, name, NULL, 0, digest,
        0u, TREE_DIR_REMOVE, removed_copy, &replacement);
    if (status != INFS_STATUS_OK)
        return status;
    payload->entry_count = infs_cpu_to_le32(count - 1u);
    payload->bytes_used = 0;
    tree_dir_set_root(payload, replacement);
    header->payload_size = infs_cpu_to_le32(
        (uint32_t)(sizeof(*payload) + sizeof(uint64_t)));
    directory_cache_remove(vol, header->object_id, name);
    return INFS_STATUS_OK;
}
