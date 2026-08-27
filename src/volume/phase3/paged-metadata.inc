// SPDX-License-Identifier: GPL-3.0-or-later
/* Format 0.8 scalable metadata pages.
 *
 * Directory and object-index head objects remain normal persistent objects,
 * preserving their stable IDs and the existing transaction model. Their
 * variable-size entry sets live in independently checksummed metadata pages.
 * The head stores a compact array of physical page pointers. Page updates are
 * copy-on-write when the old page belongs to the committed generation, while
 * pages created earlier in the current transaction may be updated in place.
 */

static int metadata_page_generation_valid(struct infs_volume *vol,
                                          const uint8_t block[INFS_BLOCK_SIZE])
{
    const struct infs_metadata_page_disk *page =
        (const struct infs_metadata_page_disk *)block;
    uint64_t generation = infs_le64_to_cpu(page->generation);
    uint64_t limit = vol->tx_active ?
        infs_le64_to_cpu(vol->tx_base_sb.generation) + 1u :
        infs_le64_to_cpu(vol->sb.generation);
    return generation != 0 && generation <= limit;
}

static uint64_t *metadata_head_page_pointers(void *payload)
{
    return (uint64_t *)((uint8_t *)payload +
                        (payload ? 0u : 0u));
}

static uint64_t *directory_page_pointers(
    struct infs_directory_payload_disk *payload)
{
    return (uint64_t *)(payload + 1);
}

static uint64_t *index_page_pointers(struct infs_index_payload_disk *payload)
{
    return (uint64_t *)(payload + 1);
}

static int index_page_validate(struct infs_volume *vol,
                               const uint8_t block[INFS_BLOCK_SIZE],
                               const uint8_t owner_id[16],
                               struct infs_index_entry_disk **entries_out,
                               uint32_t *count_out)
{
    if (!infs_validate_metadata_page(block, INFS_INDEX_PAGE_MAGIC, owner_id) ||
        !metadata_page_generation_valid(vol, block))
        return INFS_STATUS_CORRUPT;
    const struct infs_metadata_page_disk *page =
        (const struct infs_metadata_page_disk *)block;
    uint32_t count = infs_le32_to_cpu(page->entry_count);
    uint32_t bytes = infs_le32_to_cpu(page->bytes_used);
    if (count > INFS_INDEX_ENTRIES_PER_PAGE ||
        bytes != count * sizeof(struct infs_index_entry_disk))
        return INFS_STATUS_CORRUPT;

    struct infs_index_entry_disk *entries =
        (struct infs_index_entry_disk *)(
            (uint8_t *)block + sizeof(struct infs_metadata_page_disk));
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t type = infs_le16_to_cpu(entries[i].object_type);
        if (id_is_zero(entries[i].object_id) ||
            infs_le16_to_cpu(entries[i].flags) != 0 ||
            infs_le32_to_cpu(entries[i].reserved) != 0 ||
            (type != INFS_OBJECT_DIRECTORY && type != INFS_OBJECT_FILE &&
             type != INFS_OBJECT_CHECKSUM && type != INFS_OBJECT_SYMLINK &&
             type != INFS_OBJECT_SNAPSHOT_CATALOG) ||
            (type == INFS_OBJECT_SYMLINK && !symbolic_links_enabled(vol)) ||
            (type == INFS_OBJECT_SNAPSHOT_CATALOG &&
             (!snapshots_enabled(vol) ||
              memcmp(entries[i].object_id, snapshot_catalog_id, 16) != 0)))
            return INFS_STATUS_CORRUPT;
    }
    if (entries_out)
        *entries_out = entries;
    if (count_out)
        *count_out = count;
    return INFS_STATUS_OK;
}

static int directory_page_validate(struct infs_volume *vol,
                                   const uint8_t block[INFS_BLOCK_SIZE],
                                   const uint8_t owner_id[16],
                                   uint8_t **entries_out,
                                   uint32_t *bytes_out,
                                   uint32_t *count_out)
{
    if (!infs_validate_metadata_page(block, INFS_DIRECTORY_PAGE_MAGIC,
                                     owner_id) ||
        !metadata_page_generation_valid(vol, block))
        return INFS_STATUS_CORRUPT;
    const struct infs_metadata_page_disk *page =
        (const struct infs_metadata_page_disk *)block;
    uint32_t bytes = infs_le32_to_cpu(page->bytes_used);
    uint32_t expected_count = infs_le32_to_cpu(page->entry_count);
    uint8_t *cursor = (uint8_t *)block + sizeof(*page);
    uint32_t remaining = bytes;
    uint32_t seen = 0;
    while (remaining) {
        if (remaining < sizeof(struct infs_dirent_disk))
            return INFS_STATUS_CORRUPT;
        struct infs_dirent_disk *entry = (struct infs_dirent_disk *)cursor;
        uint16_t record_size = infs_le16_to_cpu(entry->record_size);
        uint16_t name_length = infs_le16_to_cpu(entry->name_length);
        uint16_t type = infs_le16_to_cpu(entry->object_type);
        const uint8_t *name = cursor + sizeof(*entry);
        if (record_size < sizeof(*entry) || record_size > remaining ||
            (record_size & 7u) != 0 || name_length == 0 ||
            name_length > INFS_NAME_MAX ||
            sizeof(*entry) + name_length > record_size ||
            !namespace_object_type(type) ||
            (type == INFS_OBJECT_SYMLINK && !symbolic_links_enabled(vol)) ||
            infs_le16_to_cpu(entry->flags) != 0 ||
            id_is_zero(entry->object_id) ||
            memchr(name, '\0', name_length) != NULL ||
            memchr(name, '/', name_length) != NULL ||
            !infs_utf8_validate(name, name_length) ||
            (name_length == 1u && name[0] == '.') ||
            (name_length == 2u && name[0] == '.' && name[1] == '.'))
            return INFS_STATUS_CORRUPT;
        for (size_t i = sizeof(*entry) + name_length; i < record_size; ++i)
            if (cursor[i] != 0)
                return INFS_STATUS_CORRUPT;
        cursor += record_size;
        remaining -= record_size;
        ++seen;
    }
    if (seen != expected_count)
        return INFS_STATUS_CORRUPT;
    if (entries_out)
        *entries_out = (uint8_t *)block + sizeof(*page);
    if (bytes_out)
        *bytes_out = bytes;
    if (count_out)
        *count_out = seen;
    return INFS_STATUS_OK;
}

static int metadata_page_store(struct infs_volume *vol, uint64_t old_block,
                               uint8_t block[INFS_BLOCK_SIZE],
                               uint64_t *new_block_out)
{
    if (!vol || !new_block_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    infs_status status = tx_begin(vol);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_metadata_page_disk *page =
        (struct infs_metadata_page_disk *)block;
    page->generation = infs_cpu_to_le64(
        infs_le64_to_cpu(vol->tx_base_sb.generation) + 1u);
    status = infs_metadata_page_finalize(block);
    if (status != INFS_STATUS_OK)
        return status;

    if (old_block != 0 && old_block < infs_le64_to_cpu(vol->sb.total_blocks) &&
        !bitmap_get(tx_cow_base_bitmap(vol), old_block)) {
        status = write_block(vol, old_block, block);
        if (status != INFS_STATUS_OK)
            return status;
        *new_block_out = old_block;
        return INFS_STATUS_OK;
    }

    uint64_t new_block = 0, got = 0;
    status = alloc_run(vol, 1, &new_block, &got);
    if (status != INFS_STATUS_OK)
        return status;
    if (got != 1)
        return INFS_STATUS_CORRUPT;
    status = write_block(vol, new_block, block);
    if (status != INFS_STATUS_OK) {
        free_run(vol, new_block, 1);
        return status;
    }
    if (old_block != 0)
        free_run(vol, old_block, 1);
    if (vol->tx_error != INFS_STATUS_OK)
        return vol->tx_error;
    *new_block_out = new_block;
    return INFS_STATUS_OK;
}

static int paged_index_find(struct infs_volume *vol, const uint8_t id[16],
                            struct infs_lookup *out)
{
    uint8_t head[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    infs_status status = index_load(vol, head, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)head;
    if (infs_le16_to_cpu(header->object_version) !=
        INFS_OBJECT_VERSION_PAGED)
        return INFS_STATUS_CORRUPT;

    uint32_t page_count = infs_le32_to_cpu(payload->reserved);
    uint64_t *pages = index_page_pointers(payload);
    for (uint32_t p = 0; p < page_count; ++p) {
        uint64_t page_block = infs_le64_to_cpu(pages[p]);
        if (page_block == 0 ||
            page_block >= infs_le64_to_cpu(vol->sb.total_blocks) ||
            !bitmap_get(vol->bitmap, page_block))
            return INFS_STATUS_CORRUPT;
        uint8_t page_data[INFS_BLOCK_SIZE];
        status = read_block(vol, page_block, page_data);
        if (status != INFS_STATUS_OK)
            return status;
        struct infs_index_entry_disk *entries;
        uint32_t count;
        status = index_page_validate(vol, page_data, header->object_id,
                                     &entries, &count);
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
    }
    return INFS_STATUS_NOT_FOUND;
}

static int paged_index_repoint(struct infs_volume *vol, const uint8_t id[16],
                               uint64_t object_block, uint16_t type)
{
    uint8_t head[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    uint64_t head_block = infs_le64_to_cpu(vol->sb.object_index_block);
    infs_status status = index_load(vol, head, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)head;
    uint32_t page_count = infs_le32_to_cpu(payload->reserved);
    uint64_t *pages = index_page_pointers(payload);

    for (uint32_t p = 0; p < page_count; ++p) {
        uint64_t page_block = infs_le64_to_cpu(pages[p]);
        uint8_t page_data[INFS_BLOCK_SIZE];
        status = read_block(vol, page_block, page_data);
        if (status != INFS_STATUS_OK)
            return status;
        struct infs_index_entry_disk *entries;
        uint32_t count;
        status = index_page_validate(vol, page_data, header->object_id,
                                     &entries, &count);
        if (status != INFS_STATUS_OK)
            return status;
        for (uint32_t i = 0; i < count; ++i) {
            if (memcmp(entries[i].object_id, id, 16) != 0)
                continue;
            entries[i].object_block = infs_cpu_to_le64(object_block);
            entries[i].object_type = infs_cpu_to_le16(type);
            uint64_t replacement = 0;
            status = metadata_page_store(vol, page_block, page_data,
                                         &replacement);
            if (status != INFS_STATUS_OK)
                return status;
            pages[p] = infs_cpu_to_le64(replacement);
            return write_object(vol, head_block, head);
        }
    }
    return INFS_STATUS_NOT_FOUND;
}

static int paged_index_add(struct infs_volume *vol, const uint8_t id[16],
                           uint64_t object_block, uint16_t type)
{
    struct infs_lookup existing;
    infs_status status;
    if (object_cache_lookup(vol, id, &existing))
        return INFS_STATUS_ALREADY_EXISTS;
    if (!vol->object_cache_complete) {
        status = paged_index_find(vol, id, &existing);
        if (status == INFS_STATUS_OK)
            return INFS_STATUS_ALREADY_EXISTS;
        if (status != INFS_STATUS_NOT_FOUND)
            return status;
    }

    uint8_t head[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    uint64_t head_block = infs_le64_to_cpu(vol->sb.object_index_block);
    status = index_load(vol, head, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)head;
    uint32_t page_count = infs_le32_to_cpu(payload->reserved);
    uint32_t total_count = infs_le32_to_cpu(payload->entry_count);
    uint64_t *pages = index_page_pointers(payload);

    uint8_t page_data[INFS_BLOCK_SIZE];
    uint64_t old_page = 0;
    struct infs_index_entry_disk *entries = NULL;
    uint32_t count = 0;
    if (page_count != 0) {
        old_page = infs_le64_to_cpu(pages[page_count - 1u]);
        status = read_block(vol, old_page, page_data);
        if (status != INFS_STATUS_OK)
            return status;
        status = index_page_validate(vol, page_data, header->object_id,
                                     &entries, &count);
        if (status != INFS_STATUS_OK)
            return status;
    }

    if (page_count == 0 || count == INFS_INDEX_ENTRIES_PER_PAGE) {
        if (page_count >= INFS_INDEX_PAGE_POINTERS)
            return INFS_STATUS_NO_SPACE;
        status = infs_metadata_page_init(
            page_data, INFS_INDEX_PAGE_MAGIC, header->object_id,
            infs_le64_to_cpu(vol->tx_base_sb.generation) + 1u);
        if (status != INFS_STATUS_OK)
            return status;
        struct infs_metadata_page_disk *page =
            (struct infs_metadata_page_disk *)page_data;
        entries = (struct infs_index_entry_disk *)(page + 1);
        count = 0;
        old_page = 0;
    }

    struct infs_metadata_page_disk *page =
        (struct infs_metadata_page_disk *)page_data;
    struct infs_index_entry_disk *entry = &entries[count];
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->object_id, id, 16);
    entry->object_block = infs_cpu_to_le64(object_block);
    entry->object_type = infs_cpu_to_le16(type);
    page->entry_count = infs_cpu_to_le32(count + 1u);
    page->bytes_used = infs_cpu_to_le32(
        (count + 1u) * sizeof(struct infs_index_entry_disk));

    uint64_t replacement = 0;
    status = metadata_page_store(vol, old_page, page_data, &replacement);
    if (status != INFS_STATUS_OK)
        return status;
    if (old_page == 0) {
        pages[page_count] = infs_cpu_to_le64(replacement);
        ++page_count;
        payload->reserved = infs_cpu_to_le32(page_count);
    } else {
        pages[page_count - 1u] = infs_cpu_to_le64(replacement);
    }
    payload->entry_count = infs_cpu_to_le32(total_count + 1u);
    header->payload_size = infs_cpu_to_le32((uint32_t)(
        sizeof(*payload) + (size_t)page_count * sizeof(uint64_t)));
    status = write_object(vol, head_block, head);
    if (status == INFS_STATUS_OK)
        (void)object_cache_store_page(vol, id, object_block, type,
                                      page_count - 1u);
    return status;
}

static int paged_index_remove(struct infs_volume *vol, const uint8_t id[16])
{
    uint8_t head[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    uint64_t head_block = infs_le64_to_cpu(vol->sb.object_index_block);
    infs_status status = index_load(vol, head, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)head;
    uint32_t page_count = infs_le32_to_cpu(payload->reserved);
    uint32_t total_count = infs_le32_to_cpu(payload->entry_count);
    uint64_t *pages = index_page_pointers(payload);

    for (uint32_t p = 0; p < page_count; ++p) {
        uint64_t page_block = infs_le64_to_cpu(pages[p]);
        uint8_t page_data[INFS_BLOCK_SIZE];
        status = read_block(vol, page_block, page_data);
        if (status != INFS_STATUS_OK)
            return status;
        struct infs_index_entry_disk *entries;
        uint32_t count;
        status = index_page_validate(vol, page_data, header->object_id,
                                     &entries, &count);
        if (status != INFS_STATUS_OK)
            return status;
        for (uint32_t i = 0; i < count; ++i) {
            if (memcmp(entries[i].object_id, id, 16) != 0)
                continue;
            if (i + 1u < count)
                memmove(&entries[i], &entries[i + 1u],
                        (size_t)(count - i - 1u) * sizeof(*entries));
            memset(&entries[count - 1u], 0, sizeof(*entries));
            struct infs_metadata_page_disk *page =
                (struct infs_metadata_page_disk *)page_data;
            page->entry_count = infs_cpu_to_le32(count - 1u);
            page->bytes_used = infs_cpu_to_le32(
                (count - 1u) * sizeof(struct infs_index_entry_disk));

            if (count == 1u) {
                free_run(vol, page_block, 1);
                if (p + 1u < page_count)
                    memmove(&pages[p], &pages[p + 1u],
                            (size_t)(page_count - p - 1u) * sizeof(*pages));
                pages[page_count - 1u] = 0;
                --page_count;
                payload->reserved = infs_cpu_to_le32(page_count);
            } else {
                uint64_t replacement = 0;
                status = metadata_page_store(vol, page_block, page_data,
                                             &replacement);
                if (status != INFS_STATUS_OK)
                    return status;
                pages[p] = infs_cpu_to_le64(replacement);
            }
            if (total_count == 0)
                return INFS_STATUS_CORRUPT;
            payload->entry_count = infs_cpu_to_le32(total_count - 1u);
            header->payload_size = infs_cpu_to_le32((uint32_t)(
                sizeof(*payload) + (size_t)page_count * sizeof(uint64_t)));
            return write_object(vol, head_block, head);
        }
    }
    return INFS_STATUS_NOT_FOUND;
}

static int index_snapshot(struct infs_volume *vol,
                          struct infs_index_entry_disk **entries_out,
                          uint32_t *count_out)
{
    if (!entries_out || !count_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    *entries_out = NULL;
    *count_out = 0;

    uint8_t head[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    infs_status status = index_load(vol, head, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)head;
    uint32_t total_count = infs_le32_to_cpu(payload->entry_count);
    struct infs_index_entry_disk *snapshot = calloc(
        total_count ? total_count : 1u, sizeof(*snapshot));
    if (!snapshot)
        return INFS_STATUS_NO_MEMORY;

    if (infs_le16_to_cpu(header->object_version) ==
        INFS_OBJECT_VERSION_TREE) {
        free(snapshot);
        return tree_index_snapshot(vol, entries_out, count_out);
    }

    if (infs_le16_to_cpu(header->object_version) ==
        INFS_OBJECT_VERSION_CLASSIC) {
        memcpy(snapshot, payload + 1,
               (size_t)total_count * sizeof(*snapshot));
        *entries_out = snapshot;
        *count_out = total_count;
        return INFS_STATUS_OK;
    }

    uint32_t page_count = infs_le32_to_cpu(payload->reserved);
    uint64_t *pages = index_page_pointers(payload);
    uint32_t copied = 0;
    for (uint32_t p = 0; p < page_count; ++p) {
        uint64_t page_block = infs_le64_to_cpu(pages[p]);
        uint8_t page_data[INFS_BLOCK_SIZE];
        status = read_block(vol, page_block, page_data);
        if (status != INFS_STATUS_OK)
            goto fail;
        struct infs_index_entry_disk *entries;
        uint32_t count;
        status = index_page_validate(vol, page_data, header->object_id,
                                     &entries, &count);
        if (status != INFS_STATUS_OK || count > total_count - copied) {
            status = INFS_STATUS_CORRUPT;
            goto fail;
        }
        memcpy(&snapshot[copied], entries, (size_t)count * sizeof(*entries));
        copied += count;
    }
    if (copied != total_count) {
        status = INFS_STATUS_CORRUPT;
        goto fail;
    }
    *entries_out = snapshot;
    *count_out = total_count;
    return INFS_STATUS_OK;

fail:
    free(snapshot);
    return status;
}


static int object_cache_rebuild(struct infs_volume *vol)
{
    if (!vol || !vol->object_cache || !vol->object_cache_slots)
        return INFS_STATUS_OK;
    object_cache_clear(vol);
    uint8_t head[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    infs_status status = index_load(vol, head, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)head;
    if (infs_le16_to_cpu(header->object_version) == INFS_OBJECT_VERSION_TREE) {
        struct infs_index_entry_disk *entries = NULL;
        uint32_t count = 0;
        status = tree_index_snapshot(vol, &entries, &count);
        if (status != INFS_STATUS_OK)
            goto fail;
        for (uint32_t i = 0; i < count; ++i) {
            if (!object_cache_store_page(
                    vol, entries[i].object_id,
                    infs_le64_to_cpu(entries[i].object_block),
                    infs_le16_to_cpu(entries[i].object_type),
                    INFS_INDEX_PAGE_UNKNOWN)) {
                free(entries);
                goto capacity;
            }
        }
        free(entries);
    } else if (infs_le16_to_cpu(header->object_version) == INFS_OBJECT_VERSION_PAGED) {
        uint32_t page_count = infs_le32_to_cpu(payload->reserved);
        uint64_t *pages = index_page_pointers(payload);
        for (uint32_t p = 0; p < page_count; ++p) {
            uint8_t page_data[INFS_BLOCK_SIZE];
            status = read_block(vol, infs_le64_to_cpu(pages[p]), page_data);
            if (status != INFS_STATUS_OK)
                goto fail;
            struct infs_index_entry_disk *entries;
            uint32_t count;
            status = index_page_validate(vol, page_data, header->object_id,
                                         &entries, &count);
            if (status != INFS_STATUS_OK)
                goto fail;
            for (uint32_t i = 0; i < count; ++i) {
                if (!object_cache_store_page(
                        vol, entries[i].object_id,
                        infs_le64_to_cpu(entries[i].object_block),
                        infs_le16_to_cpu(entries[i].object_type), p))
                    goto capacity;
            }
        }
    } else {
        uint32_t count = infs_le32_to_cpu(payload->entry_count);
        struct infs_index_entry_disk *entries =
            (struct infs_index_entry_disk *)(payload + 1);
        for (uint32_t i = 0; i < count; ++i) {
            if (!object_cache_store_page(
                    vol, entries[i].object_id,
                    infs_le64_to_cpu(entries[i].object_block),
                    infs_le16_to_cpu(entries[i].object_type),
                    INFS_INDEX_PAGE_UNKNOWN))
                goto capacity;
        }
    }
    vol->object_cache_complete = 1;
    return INFS_STATUS_OK;
capacity:
    object_cache_clear(vol);
    return INFS_STATUS_OK;
fail:
    object_cache_clear(vol);
    return status;
}

static int paged_dir_lookup(struct infs_volume *vol,
                            uint8_t head[INFS_BLOCK_SIZE], const char *name,
                            struct infs_dirent_disk *entry_out)
{
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)head;
    struct infs_directory_payload_disk *payload =
        (struct infs_directory_payload_disk *)(head + sizeof(*header));
    if (directory_cache_lookup(vol, header->object_id, name, entry_out))
        return INFS_STATUS_OK;
    if (directory_cache_complete(vol, header->object_id))
        return INFS_STATUS_NOT_FOUND;

    size_t wanted_length = strlen(name);
    uint32_t page_count = infs_le32_to_cpu(payload->bytes_used);
    uint64_t *pages = directory_page_pointers(payload);
    struct infs_dirent_disk wanted;
    int have_wanted = 0;
    int cached_all = 1;
    for (uint32_t p = 0; p < page_count; ++p) {
        uint64_t page_block = infs_le64_to_cpu(pages[p]);
        if (page_block == 0 ||
            page_block >= infs_le64_to_cpu(vol->sb.total_blocks) ||
            !bitmap_get(vol->bitmap, page_block))
            return INFS_STATUS_CORRUPT;
        uint8_t page_data[INFS_BLOCK_SIZE];
        infs_status status = read_block(vol, page_block, page_data);
        if (status != INFS_STATUS_OK)
            return status;
        uint8_t *entries;
        uint32_t bytes;
        status = directory_page_validate(vol, page_data, header->object_id,
                                         &entries, &bytes, NULL);
        if (status != INFS_STATUS_OK)
            return status;
        uint32_t offset = 0;
        while (offset < bytes) {
            struct infs_dirent_disk *entry =
                (struct infs_dirent_disk *)(entries + offset);
            uint16_t record_size = infs_le16_to_cpu(entry->record_size);
            uint16_t stored_length = infs_le16_to_cpu(entry->name_length);
            char cached_name[INFS_NAME_MAX + 1u];
            memcpy(cached_name, entries + offset + sizeof(*entry), stored_length);
            cached_name[stored_length] = '\0';
            if (!directory_cache_store(vol, header->object_id, cached_name,
                                       entry->object_id,
                                       infs_le16_to_cpu(entry->object_type), p))
                cached_all = 0;
            if (!have_wanted && stored_length == wanted_length &&
                memcmp(cached_name, name, wanted_length) == 0) {
                wanted = *entry;
                have_wanted = 1;
            }
            offset += record_size;
        }
    }
    if (cached_all)
        directory_cache_mark_complete(vol, header->object_id);
    if (!have_wanted)
        return INFS_STATUS_NOT_FOUND;
    if (entry_out)
        *entry_out = wanted;
    return INFS_STATUS_OK;
}

static int paged_dir_add(struct infs_volume *vol,
                         uint8_t head[INFS_BLOCK_SIZE], const char *name,
                         const uint8_t object_id[16], uint16_t type)
{
    size_t name_length = strlen(name);
    if (name_length == 0 || name_length > INFS_NAME_MAX || strchr(name, '/') ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
        id_is_zero(object_id) || !infs_utf8_validate(name, name_length) ||
        !namespace_object_type(type))
        return INFS_STATUS_INVALID_ARGUMENT;
    infs_status status = paged_dir_lookup(vol, head, name, NULL);
    if (status == INFS_STATUS_OK)
        return INFS_STATUS_ALREADY_EXISTS;
    if (status != INFS_STATUS_NOT_FOUND)
        return status;

    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)head;
    struct infs_directory_payload_disk *payload =
        (struct infs_directory_payload_disk *)(head + sizeof(*header));
    uint32_t page_count = infs_le32_to_cpu(payload->bytes_used);
    uint64_t *pages = directory_page_pointers(payload);
    size_t record_size = align8(sizeof(struct infs_dirent_disk) + name_length);

    uint8_t page_data[INFS_BLOCK_SIZE];
    uint64_t old_page = 0;
    uint8_t *entries = NULL;
    uint32_t bytes = 0;
    uint32_t count = 0;
    if (page_count != 0) {
        old_page = infs_le64_to_cpu(pages[page_count - 1u]);
        status = read_block(vol, old_page, page_data);
        if (status != INFS_STATUS_OK)
            return status;
        status = directory_page_validate(vol, page_data, header->object_id,
                                         &entries, &bytes, &count);
        if (status != INFS_STATUS_OK)
            return status;
    }

    if (page_count == 0 ||
        record_size > INFS_METADATA_PAGE_DATA_SIZE - bytes) {
        if (page_count >= INFS_DIRECTORY_PAGE_POINTERS)
            return INFS_STATUS_NO_SPACE;
        status = infs_metadata_page_init(
            page_data, INFS_DIRECTORY_PAGE_MAGIC, header->object_id,
            infs_le64_to_cpu(vol->tx_base_sb.generation) + 1u);
        if (status != INFS_STATUS_OK)
            return status;
        struct infs_metadata_page_disk *page =
            (struct infs_metadata_page_disk *)page_data;
        entries = (uint8_t *)(page + 1);
        bytes = 0;
        count = 0;
        old_page = 0;
    }

    uint8_t *cursor = entries + bytes;
    memset(cursor, 0, record_size);
    struct infs_dirent_disk *entry = (struct infs_dirent_disk *)cursor;
    entry->record_size = infs_cpu_to_le16((uint16_t)record_size);
    entry->name_length = infs_cpu_to_le16((uint16_t)name_length);
    entry->object_type = infs_cpu_to_le16(type);
    memcpy(entry->object_id, object_id, 16);
    memcpy(cursor + sizeof(*entry), name, name_length);
    struct infs_metadata_page_disk *page =
        (struct infs_metadata_page_disk *)page_data;
    page->entry_count = infs_cpu_to_le32(count + 1u);
    page->bytes_used = infs_cpu_to_le32((uint32_t)(bytes + record_size));

    uint64_t replacement = 0;
    status = metadata_page_store(vol, old_page, page_data, &replacement);
    if (status != INFS_STATUS_OK)
        return status;
    if (old_page == 0) {
        pages[page_count] = infs_cpu_to_le64(replacement);
        ++page_count;
        payload->bytes_used = infs_cpu_to_le32(page_count);
    } else {
        pages[page_count - 1u] = infs_cpu_to_le64(replacement);
    }
    payload->entry_count = infs_cpu_to_le32(
        infs_le32_to_cpu(payload->entry_count) + 1u);
    header->payload_size = infs_cpu_to_le32((uint32_t)(
        sizeof(*payload) + (size_t)page_count * sizeof(uint64_t)));
    (void)directory_cache_store(vol, header->object_id, name, object_id, type,
                                page_count - 1u);
    return INFS_STATUS_OK;
}

static int paged_dir_remove(struct infs_volume *vol,
                            uint8_t head[INFS_BLOCK_SIZE], const char *name,
                            struct infs_dirent_disk *removed_copy)
{
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)head;
    struct infs_directory_payload_disk *payload =
        (struct infs_directory_payload_disk *)(head + sizeof(*header));
    size_t name_length = strlen(name);
    uint32_t page_count = infs_le32_to_cpu(payload->bytes_used);
    uint64_t *pages = directory_page_pointers(payload);
    uint32_t hinted_page = INFS_DIRECTORY_PAGE_UNKNOWN;
    (void)directory_cache_page(vol, header->object_id, name, &hinted_page);

    for (uint32_t pass = 0; pass < (hinted_page < page_count ? 2u : 1u); ++pass) {
      uint32_t begin = (pass == 0 && hinted_page < page_count) ? hinted_page : 0u;
      uint32_t finish = (pass == 0 && hinted_page < page_count) ? hinted_page + 1u : page_count;
      for (uint32_t p = begin; p < finish; ++p) {
        if (pass == 1 && p == hinted_page)
            continue;
        uint64_t page_block = infs_le64_to_cpu(pages[p]);
        uint8_t page_data[INFS_BLOCK_SIZE];
        infs_status status = read_block(vol, page_block, page_data);
        if (status != INFS_STATUS_OK)
            return status;
        uint8_t *entries;
        uint32_t bytes, count;
        status = directory_page_validate(vol, page_data, header->object_id,
                                         &entries, &bytes, &count);
        if (status != INFS_STATUS_OK)
            return status;
        uint32_t offset = 0;
        while (offset < bytes) {
            struct infs_dirent_disk *entry =
                (struct infs_dirent_disk *)(entries + offset);
            uint16_t record_size = infs_le16_to_cpu(entry->record_size);
            uint16_t stored_length = infs_le16_to_cpu(entry->name_length);
            if (stored_length == name_length &&
                memcmp(entries + offset + sizeof(*entry), name,
                       name_length) == 0) {
                if (removed_copy)
                    memcpy(removed_copy, entry, sizeof(*removed_copy));
                memmove(entries + offset, entries + offset + record_size,
                        bytes - offset - record_size);
                memset(entries + bytes - record_size, 0, record_size);
                struct infs_metadata_page_disk *page =
                    (struct infs_metadata_page_disk *)page_data;
                page->entry_count = infs_cpu_to_le32(count - 1u);
                page->bytes_used = infs_cpu_to_le32(bytes - record_size);

                if (count == 1u) {
                    free_run(vol, page_block, 1);
                    directory_cache_invalidate_parent(vol, header->object_id);
                    if (p + 1u < page_count)
                        memmove(&pages[p], &pages[p + 1u],
                                (size_t)(page_count - p - 1u) * sizeof(*pages));
                    pages[page_count - 1u] = 0;
                    --page_count;
                    payload->bytes_used = infs_cpu_to_le32(page_count);
                } else {
                    uint64_t replacement = 0;
                    status = metadata_page_store(vol, page_block, page_data,
                                                 &replacement);
                    if (status != INFS_STATUS_OK)
                        return status;
                    pages[p] = infs_cpu_to_le64(replacement);
                    directory_cache_remove(vol, header->object_id, name);
                }
                uint32_t total = infs_le32_to_cpu(payload->entry_count);
                if (total == 0)
                    return INFS_STATUS_CORRUPT;
                payload->entry_count = infs_cpu_to_le32(total - 1u);
                header->payload_size = infs_cpu_to_le32((uint32_t)(
                    sizeof(*payload) +
                    (size_t)page_count * sizeof(uint64_t)));
                return INFS_STATUS_OK;
            }
            offset += record_size;
        }
      }
    }
    return INFS_STATUS_NOT_FOUND;
}

static int paged_dir_snapshot(struct infs_volume *vol,
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
        (struct infs_directory_payload_disk *)(head + sizeof(*header));
    uint32_t page_count = infs_le32_to_cpu(payload->bytes_used);
    uint32_t expected_count = infs_le32_to_cpu(payload->entry_count);
    uint64_t *pages = directory_page_pointers(payload);

    size_t total_bytes = 0;
    uint32_t total_count = 0;
    for (uint32_t p = 0; p < page_count; ++p) {
        uint8_t page_data[INFS_BLOCK_SIZE];
        uint64_t page_block = infs_le64_to_cpu(pages[p]);
        infs_status status = read_block(vol, page_block, page_data);
        if (status != INFS_STATUS_OK)
            return status;
        uint32_t bytes, count;
        status = directory_page_validate(vol, page_data, header->object_id,
                                         NULL, &bytes, &count);
        if (status != INFS_STATUS_OK || SIZE_MAX - total_bytes < bytes ||
            UINT32_MAX - total_count < count)
            return status == INFS_STATUS_OK ? INFS_STATUS_OVERFLOW : status;
        total_bytes += bytes;
        total_count += count;
    }
    if (total_count != expected_count)
        return INFS_STATUS_CORRUPT;

    uint8_t *snapshot = malloc(total_bytes ? total_bytes : 1u);
    if (!snapshot)
        return INFS_STATUS_NO_MEMORY;
    size_t copied = 0;
    for (uint32_t p = 0; p < page_count; ++p) {
        uint8_t page_data[INFS_BLOCK_SIZE];
        uint64_t page_block = infs_le64_to_cpu(pages[p]);
        infs_status status = read_block(vol, page_block, page_data);
        if (status != INFS_STATUS_OK) {
            free(snapshot);
            return status;
        }
        uint8_t *entries;
        uint32_t bytes;
        status = directory_page_validate(vol, page_data, header->object_id,
                                         &entries, &bytes, NULL);
        if (status != INFS_STATUS_OK) {
            free(snapshot);
            return status;
        }
        memcpy(snapshot + copied, entries, bytes);
        copied += bytes;
    }
    *bytes_out = snapshot;
    *size_out = total_bytes;
    *count_out = total_count;
    return INFS_STATUS_OK;
}
