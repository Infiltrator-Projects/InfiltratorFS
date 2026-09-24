// SPDX-License-Identifier: GPL-3.0-or-later

struct snapshot_visit_entry {
    uint64_t generation;
    uint64_t object_index_block;
};

struct snapshot_visit_context {
    struct snapshot_visit_entry *entries;
    size_t count;
    size_t capacity;
};

static void snapshot_visit_context_destroy(struct snapshot_visit_context *ctx)
{
    if (!ctx)
        return;
    free(ctx->entries);
    free(ctx);
}

static int snapshot_visit_once(
    struct snapshot_visit_context *ctx,
    const struct infs_snapshot_record_disk *record, int *first_visit)
{
    if (!ctx || !record || !first_visit)
        return INFS_STATUS_INVALID_ARGUMENT;
    uint64_t generation = infs_le64_to_cpu(record->generation);
    uint64_t index = infs_le64_to_cpu(record->object_index_block);
    for (size_t i = 0; i < ctx->count; ++i) {
        if (ctx->entries[i].generation == generation &&
            ctx->entries[i].object_index_block == index) {
            *first_visit = 0;
            return INFS_STATUS_OK;
        }
    }
    size_t required = 0;
    if (!infiltratr_size_add_checked(ctx->count, 1u, &required))
        return INFS_STATUS_OVERFLOW;
    if (!infiltratr_array_reserve(
            (void **)&ctx->entries, &ctx->capacity, sizeof(*ctx->entries),
            required, 32u))
        return INFS_STATUS_NO_MEMORY;
    ctx->entries[ctx->count].generation = generation;
    ctx->entries[ctx->count].object_index_block = index;
    ++ctx->count;
    *first_visit = 1;
    return INFS_STATUS_OK;
}

static int snapshot_name_valid(const char *name, size_t *length_out)
{
    if (!name)
        return 0;
    size_t length = 0;
    while (length <= INFS_SNAPSHOT_NAME_MAX && name[length] != '\0')
        ++length;
    if (length == 0 || length > INFS_SNAPSHOT_NAME_MAX ||
        strchr(name, '/') || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0 || !infs_utf8_validate(name, length))
        return 0;
    if (length_out)
        *length_out = length;
    return 1;
}

static int snapshot_record_name_equal(
    const struct infs_snapshot_record_disk *record, const char *name,
    size_t length)
{
    return infs_le16_to_cpu(record->name_length) == length &&
        memcmp(record->name, name, length) == 0;
}

static int snapshot_record_valid(
    struct infs_volume *vol, const struct infs_snapshot_record_disk *record,
    uint64_t catalog_generation)
{
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    uint64_t generation = infs_le64_to_cpu(record->generation);
    uint64_t allocation_root =
        infs_le64_to_cpu(record->allocation_root_block);
    uint64_t allocation_leaves =
        infs_le64_to_cpu(record->allocation_leaf_count);
    uint64_t index = infs_le64_to_cpu(record->object_index_block);
    uint64_t root = infs_le64_to_cpu(record->root_object_block);
    uint16_t name_length = infs_le16_to_cpu(record->name_length);
    size_t expected_leaves = 0;

    if (allocation_map_counts(total, &expected_leaves, NULL, NULL, NULL) !=
            INFS_STATUS_OK ||
        catalog_generation == 0 || generation == 0 ||
        generation >= catalog_generation ||
        allocation_root == 0 || allocation_root >= total ||
        allocation_leaves != expected_leaves ||
        index == 0 || index >= total || root == 0 || root >= total ||
        infs_le64_to_cpu(record->free_blocks) > total ||
        id_is_zero(record->root_object_id) ||
        !timestamp_disk_valid(&record->created_time) || name_length == 0 ||
        name_length > INFS_SNAPSHOT_NAME_MAX ||
        infs_le16_to_cpu(record->flags) != 0 ||
        infs_le32_to_cpu(record->reserved) != 0 ||
        memchr(record->name, '\0', name_length) != NULL ||
        memchr(record->name, '/', name_length) != NULL ||
        !infs_utf8_validate((const char *)record->name, name_length))
        return INFS_STATUS_CORRUPT;
    for (size_t i = name_length; i < sizeof(record->name); ++i) {
        if (record->name[i] != 0)
            return INFS_STATUS_CORRUPT;
    }
    return INFS_STATUS_OK;
}

static int snapshot_catalog_load(
    struct infs_volume *vol, uint8_t block[INFS_BLOCK_SIZE],
    struct infs_snapshot_catalog_payload_disk **payload_out,
    struct infs_lookup *lookup_out)
{
    if (!snapshots_enabled(vol))
        return INFS_STATUS_NOT_SUPPORTED;
    struct infs_lookup lookup;
    infs_status status = index_find(vol, snapshot_catalog_id, &lookup);
    if (status != INFS_STATUS_OK)
        return status;
    if (lookup.type != INFS_OBJECT_SNAPSHOT_CATALOG)
        return INFS_STATUS_CORRUPT;
    status = read_object(vol, lookup.block, block);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)block;
    if (infs_le16_to_cpu(header->object_type) !=
            INFS_OBJECT_SNAPSHOT_CATALOG ||
        infs_le16_to_cpu(header->object_version) != INFS_OBJECT_VERSION_PAGED ||
        memcmp(header->object_id, snapshot_catalog_id, 16) != 0 ||
        !id_is_zero(header->parent_id))
        return INFS_STATUS_CORRUPT;

    struct infs_snapshot_catalog_payload_disk *payload =
        (struct infs_snapshot_catalog_payload_disk *)(header + 1);
    uint32_t count = infs_le32_to_cpu(payload->snapshot_count);
    uint32_t pages = infs_le32_to_cpu(payload->reserved);
    uint32_t expected_pages = count ?
        (count + INFS_SNAPSHOT_RECORDS_PER_PAGE - 1u) /
            INFS_SNAPSHOT_RECORDS_PER_PAGE : 0u;
    if (count > INFS_SNAPSHOTS_PER_CATALOG ||
        pages != expected_pages || pages > INFS_SNAPSHOT_PAGE_POINTERS ||
        infs_le32_to_cpu(header->payload_size) !=
            sizeof(*payload) + (size_t)pages * sizeof(uint64_t))
        return INFS_STATUS_CORRUPT;

    const uint64_t *pointers = (const uint64_t *)(payload + 1);
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    for (uint32_t i = 0; i < pages; ++i) {
        uint64_t page_block = infs_le64_to_cpu(pointers[i]);
        if (!page_block || page_block >= total ||
            !bitmap_get(vol->bitmap, page_block))
            return INFS_STATUS_CORRUPT;
    }
    if (payload_out)
        *payload_out = payload;
    if (lookup_out)
        *lookup_out = lookup;
    return INFS_STATUS_OK;
}

static int snapshot_page_load(
    struct infs_volume *vol, uint64_t block,
    uint64_t catalog_generation, uint8_t page_data[INFS_BLOCK_SIZE],
    struct infs_snapshot_record_disk **records_out, uint32_t *count_out)
{
    infs_status status = read_block(vol, block, page_data);
    if (status != INFS_STATUS_OK)
        return status;
    if (!infs_validate_metadata_page(
            page_data, INFS_SNAPSHOT_PAGE_MAGIC, snapshot_catalog_id) ||
        !metadata_page_generation_valid(vol, page_data))
        return INFS_STATUS_CORRUPT;

    struct infs_metadata_page_disk *page =
        (struct infs_metadata_page_disk *)page_data;
    if (infs_le64_to_cpu(page->generation) > catalog_generation)
        return INFS_STATUS_CORRUPT;
    uint32_t count = infs_le32_to_cpu(page->entry_count);
    uint32_t bytes = infs_le32_to_cpu(page->bytes_used);
    if (!count || count > INFS_SNAPSHOT_RECORDS_PER_PAGE ||
        bytes != count * sizeof(struct infs_snapshot_record_disk))
        return INFS_STATUS_CORRUPT;

    struct infs_snapshot_record_disk *records =
        (struct infs_snapshot_record_disk *)(page + 1);
    for (uint32_t i = 0; i < count; ++i) {
        status = snapshot_record_valid(vol, &records[i], catalog_generation);
        if (status != INFS_STATUS_OK)
            return status;
    }
    if (records_out)
        *records_out = records;
    if (count_out)
        *count_out = count;
    return INFS_STATUS_OK;
}

static int snapshot_catalog_records(
    struct infs_volume *vol, const uint8_t catalog[INFS_BLOCK_SIZE],
    const struct infs_snapshot_catalog_payload_disk *payload,
    struct infs_snapshot_record_disk **records_out, uint32_t *count_out)
{
    if (!vol || !catalog || !payload || !records_out || !count_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    *records_out = NULL;
    *count_out = 0;

    const struct infs_object_header_disk *header =
        (const struct infs_object_header_disk *)catalog;
    uint64_t catalog_generation = infs_le64_to_cpu(header->generation);
    uint32_t total_count = infs_le32_to_cpu(payload->snapshot_count);
    uint32_t pages = infs_le32_to_cpu(payload->reserved);
    if (!total_count)
        return INFS_STATUS_OK;

    struct infs_snapshot_record_disk *all =
        calloc(total_count, sizeof(*all));
    if (!all)
        return INFS_STATUS_NO_MEMORY;

    const uint64_t *pointers = (const uint64_t *)(payload + 1);
    uint32_t copied = 0;
    for (uint32_t p = 0; p < pages; ++p) {
        uint8_t page_data[INFS_BLOCK_SIZE];
        struct infs_snapshot_record_disk *page_records = NULL;
        uint32_t page_count = 0;
        infs_status status = snapshot_page_load(
            vol, infs_le64_to_cpu(pointers[p]), catalog_generation,
            page_data, &page_records, &page_count);
        uint32_t expected = p + 1u < pages ?
            INFS_SNAPSHOT_RECORDS_PER_PAGE :
            total_count - p * INFS_SNAPSHOT_RECORDS_PER_PAGE;
        if (status != INFS_STATUS_OK || page_count != expected ||
            page_count > total_count - copied) {
            free(all);
            return status != INFS_STATUS_OK ? status : INFS_STATUS_CORRUPT;
        }
        memcpy(&all[copied], page_records,
               (size_t)page_count * sizeof(*all));
        copied += page_count;
    }
    if (copied != total_count) {
        free(all);
        return INFS_STATUS_CORRUPT;
    }
    for (uint32_t i = 0; i < total_count; ++i) {
        for (uint32_t j = 0; j < i; ++j) {
            uint16_t a = infs_le16_to_cpu(all[i].name_length);
            uint16_t b = infs_le16_to_cpu(all[j].name_length);
            if (a == b && memcmp(all[i].name, all[j].name, a) == 0) {
                free(all);
                return INFS_STATUS_CORRUPT;
            }
        }
    }
    *records_out = all;
    *count_out = total_count;
    return INFS_STATUS_OK;
}

static int snapshot_find_record(
    struct infs_volume *vol, const char *name,
    struct infs_snapshot_record_disk *record_out)
{
    size_t length = 0;
    if (!snapshot_name_valid(name, &length))
        return INFS_STATUS_INVALID_ARGUMENT;
    uint8_t catalog[INFS_BLOCK_SIZE];
    struct infs_snapshot_catalog_payload_disk *payload;
    infs_status status = snapshot_catalog_load(vol, catalog, &payload, NULL);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_snapshot_record_disk *records = NULL;
    uint32_t count = 0;
    status = snapshot_catalog_records(vol, catalog, payload, &records, &count);
    if (status != INFS_STATUS_OK)
        return status;
    for (uint32_t i = 0; i < count; ++i) {
        if (snapshot_record_name_equal(&records[i], name, length)) {
            if (record_out)
                *record_out = records[i];
            free(records);
            return INFS_STATUS_OK;
        }
    }
    free(records);
    return INFS_STATUS_NOT_FOUND;
}

static infs_status snapshot_load_allocation(
    struct infs_volume *vol, const struct infs_snapshot_record_disk *record,
    uint8_t **bitmap_out, struct infs_allocation_layout *layout_out)
{
    if (!vol || !record || !bitmap_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    *bitmap_out = NULL;

    struct infs_superblock_disk snapshot_sb = vol->sb;
    snapshot_sb.generation = record->generation;
    snapshot_sb.free_blocks = record->free_blocks;
    snapshot_sb.allocation_root_block = record->allocation_root_block;
    snapshot_sb.allocation_leaf_count = record->allocation_leaf_count;
    snapshot_sb.object_index_block = record->object_index_block;
    snapshot_sb.root_object_block = record->root_object_block;
    memcpy(snapshot_sb.root_object_id, record->root_object_id, 16);

    size_t bitmap_bytes = 0;
    infs_status status = allocation_map_load(
        vol, &snapshot_sb, bitmap_out, &bitmap_bytes, layout_out);
    if (status != INFS_STATUS_OK)
        return status;
    if (bitmap_bytes != vol->bitmap_bytes) {
        free(*bitmap_out);
        *bitmap_out = NULL;
        if (layout_out)
            allocation_map_layout_destroy(layout_out);
        return INFS_STATUS_CORRUPT;
    }
    return INFS_STATUS_OK;
}

static infs_status snapshot_read_bitmap(
    struct infs_volume *vol, const struct infs_snapshot_record_disk *record,
    uint8_t **bitmap_out)
{
    struct infs_allocation_layout layout = {0};
    infs_status status = snapshot_load_allocation(
        vol, record, bitmap_out, &layout);
    allocation_map_layout_destroy(&layout);
    return status;
}

static infs_status snapshot_union_bitmap(struct infs_volume *vol,
                                         uint8_t **union_out)
{
    if (!vol || !union_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    *union_out = calloc(1, vol->bitmap_bytes);
    if (!*union_out)
        return INFS_STATUS_NO_MEMORY;
    if (!snapshots_enabled(vol))
        return INFS_STATUS_OK;

    uint8_t catalog[INFS_BLOCK_SIZE];
    struct infs_snapshot_catalog_payload_disk *payload;
    infs_status status = snapshot_catalog_load(vol, catalog, &payload, NULL);
    if (status == INFS_STATUS_NOT_FOUND)
        return INFS_STATUS_OK;
    if (status != INFS_STATUS_OK) {
        free(*union_out);
        *union_out = NULL;
        return status;
    }

    struct infs_snapshot_record_disk *records = NULL;
    uint32_t count = 0;
    status = snapshot_catalog_records(vol, catalog, payload, &records, &count);
    if (status != INFS_STATUS_OK) {
        free(*union_out);
        *union_out = NULL;
        return status;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t *bitmap = NULL;
        status = snapshot_read_bitmap(vol, &records[i], &bitmap);
        if (status != INFS_STATUS_OK) {
            free(records);
            free(*union_out);
            *union_out = NULL;
            return status;
        }
        for (size_t j = 0; j < vol->bitmap_bytes; ++j)
            (*union_out)[j] |= bitmap[j];
        free(bitmap);
    }
    free(records);
    return INFS_STATUS_OK;
}

static infs_status snapshot_open_view(
    struct infs_volume *vol, const char *name, struct infs_volume *view)
{
    if (!vol || !view)
        return INFS_STATUS_INVALID_ARGUMENT;
    struct infs_snapshot_record_disk record;
    infs_status status = snapshot_find_record(vol, name, &record);
    if (status != INFS_STATUS_OK)
        return status;

    uint8_t *bitmap = NULL;
    struct infs_allocation_layout layout = {0};
    status = snapshot_load_allocation(vol, &record, &bitmap, &layout);
    if (status != INFS_STATUS_OK)
        return status;

    memset(view, 0, sizeof(*view));
    view->storage = vol->storage;
    view->size_bytes = vol->size_bytes;
    view->sb = vol->sb;
    view->sb.generation = record.generation;
    view->sb.free_blocks = record.free_blocks;
    view->sb.allocation_root_block = record.allocation_root_block;
    view->sb.allocation_leaf_count = record.allocation_leaf_count;
    view->sb.object_index_block = record.object_index_block;
    view->sb.root_object_block = record.root_object_block;
    memcpy(view->sb.root_object_id, record.root_object_id, 16);
    view->bitmap = bitmap;
    view->bitmap_bytes = vol->bitmap_bytes;
    allocation_map_cache_replace(view, &layout);
    view->data_allocation_cursor = 1u;
    view->metadata_allocation_cursor =
        infs_le64_to_cpu(view->sb.total_blocks);
    view->snapshot_validation_context = vol->snapshot_validation_context;
    view->snapshot_scrub_context = vol->snapshot_scrub_context;
    view->snapshot_view = 1;
    return INFS_STATUS_OK;
}

static void snapshot_close_view(struct infs_volume *view)
{
    if (!view)
        return;
    object_cache_destroy(view);
    allocation_map_cache_destroy(view);
    free(view->bitmap);
    view->bitmap = NULL;
}

static int snapshot_validate_catalog_graphs(struct infs_volume *vol)
{
    int owns_context = vol->snapshot_validation_context == NULL;
    struct snapshot_visit_context *visit = vol->snapshot_validation_context;
    if (owns_context) {
        visit = calloc(1, sizeof(*visit));
        if (!visit)
            return INFS_STATUS_NO_MEMORY;
        vol->snapshot_validation_context = visit;
    }

    uint8_t catalog[INFS_BLOCK_SIZE];
    struct infs_snapshot_catalog_payload_disk *payload;
    infs_status status = snapshot_catalog_load(vol, catalog, &payload, NULL);
    if (status == INFS_STATUS_NOT_FOUND) {
        status = INFS_STATUS_OK;
        goto out;
    }
    if (status != INFS_STATUS_OK)
        goto out;

    struct infs_snapshot_record_disk *records = NULL;
    uint32_t count = 0;
    status = snapshot_catalog_records(vol, catalog, payload, &records, &count);
    if (status != INFS_STATUS_OK)
        goto out;
    for (uint32_t i = 0; i < count; ++i) {
        int first_visit = 0;
        status = snapshot_visit_once(visit, &records[i], &first_visit);
        if (status != INFS_STATUS_OK)
            break;
        if (!first_visit)
            continue;
        char name[INFS_SNAPSHOT_NAME_MAX + 1u] = {0};
        uint16_t length = infs_le16_to_cpu(records[i].name_length);
        memcpy(name, records[i].name, length);
        struct infs_volume view;
        status = snapshot_open_view(vol, name, &view);
        if (status != INFS_STATUS_OK)
            break;
        status = validate_index_entries(&view);
        if (status == INFS_STATUS_OK)
            status = validate_integrity_metadata(&view);
        if (status == INFS_STATUS_OK)
            status = validate_namespace_graph(&view);
        if (status == INFS_STATUS_OK)
            status = validate_checksum_graph(&view);
        snapshot_close_view(&view);
        if (status != INFS_STATUS_OK)
            break;
    }
    free(records);
out:
    if (owns_context) {
        vol->snapshot_validation_context = NULL;
        snapshot_visit_context_destroy(visit);
    }
    return status;
}

static int snapshot_catalog_append(
    struct infs_volume *vol, const char *name, size_t name_length,
    const struct infs_timestamp *created_time)
{
    uint8_t catalog[INFS_BLOCK_SIZE];
    struct infs_snapshot_catalog_payload_disk *payload = NULL;
    struct infs_lookup lookup;
    infs_status status = snapshot_catalog_load(vol, catalog, &payload, &lookup);
    int is_new = status == INFS_STATUS_NOT_FOUND;
    if (!is_new && status != INFS_STATUS_OK)
        return status;

    if (is_new) {
        uint64_t generation = 0;
        status = transaction_next_generation(vol, &generation);
        if (status != INFS_STATUS_OK)
            return status;
        status = infs_object_init(
            catalog, INFS_OBJECT_SNAPSHOT_CATALOG, snapshot_catalog_id, NULL,
            generation, sizeof(*payload));
        if (status != INFS_STATUS_OK)
            return status;
        struct infs_object_header_disk *header =
            (struct infs_object_header_disk *)catalog;
        header->object_version = infs_cpu_to_le16(INFS_OBJECT_VERSION_PAGED);
        payload = (struct infs_snapshot_catalog_payload_disk *)(header + 1);
        memset(payload, 0, sizeof(*payload));
    } else {
        struct infs_snapshot_record_disk *existing = NULL;
        uint32_t existing_count = 0;
        status = snapshot_catalog_records(
            vol, catalog, payload, &existing, &existing_count);
        if (status != INFS_STATUS_OK)
            return status;
        for (uint32_t i = 0; i < existing_count; ++i) {
            if (snapshot_record_name_equal(&existing[i], name, name_length)) {
                free(existing);
                return INFS_STATUS_ALREADY_EXISTS;
            }
        }
        free(existing);
    }

    uint32_t count = infs_le32_to_cpu(payload->snapshot_count);
    uint32_t page_count = infs_le32_to_cpu(payload->reserved);
    if (count >= INFS_SNAPSHOTS_PER_CATALOG)
        return INFS_STATUS_NO_SPACE;

    struct infs_snapshot_record_disk record;
    memset(&record, 0, sizeof(record));
    record.generation = vol->tx_base_sb.generation;
    timestamp_disk_from_value(&record.created_time, created_time);
    record.allocation_root_block = vol->tx_base_sb.allocation_root_block;
    record.allocation_leaf_count = vol->tx_base_sb.allocation_leaf_count;
    record.free_blocks = vol->tx_base_sb.free_blocks;
    record.object_index_block = vol->tx_base_sb.object_index_block;
    record.root_object_block = vol->tx_base_sb.root_object_block;
    memcpy(record.root_object_id, vol->tx_base_sb.root_object_id, 16);
    record.name_length = infs_cpu_to_le16((uint16_t)name_length);
    memcpy(record.name, name, name_length);

    uint32_t page_index = count / INFS_SNAPSHOT_RECORDS_PER_PAGE;
    uint32_t slot = count % INFS_SNAPSHOT_RECORDS_PER_PAGE;
    uint64_t *pointers = (uint64_t *)(payload + 1);
    uint8_t page_data[INFS_BLOCK_SIZE];
    uint64_t old_page = 0;
    struct infs_snapshot_record_disk *page_records = NULL;
    uint32_t records_on_page = 0;

    if (slot == 0) {
        if (page_count >= INFS_SNAPSHOT_PAGE_POINTERS)
            return INFS_STATUS_NO_SPACE;
        uint64_t generation = 0;
        status = transaction_next_generation(vol, &generation);
        if (status != INFS_STATUS_OK)
            return status;
        status = infs_metadata_page_init(
            page_data, INFS_SNAPSHOT_PAGE_MAGIC, snapshot_catalog_id,
            generation);
        if (status != INFS_STATUS_OK)
            return status;
        struct infs_metadata_page_disk *page =
            (struct infs_metadata_page_disk *)page_data;
        page_records = (struct infs_snapshot_record_disk *)(page + 1);
        records_on_page = 0;
    } else {
        old_page = infs_le64_to_cpu(pointers[page_index]);
        status = snapshot_page_load(
            vol, old_page,
            infs_le64_to_cpu(((struct infs_object_header_disk *)catalog)->generation),
            page_data, &page_records, &records_on_page);
        if (status != INFS_STATUS_OK || records_on_page != slot)
            return status != INFS_STATUS_OK ? status : INFS_STATUS_CORRUPT;
    }

    page_records[records_on_page] = record;
    struct infs_metadata_page_disk *page =
        (struct infs_metadata_page_disk *)page_data;
    page->entry_count = infs_cpu_to_le32(records_on_page + 1u);
    page->bytes_used = infs_cpu_to_le32(
        (records_on_page + 1u) * sizeof(record));
    uint64_t replacement = 0;
    status = metadata_page_store(vol, old_page, page_data, &replacement);
    if (status != INFS_STATUS_OK)
        return status;
    pointers[page_index] = infs_cpu_to_le64(replacement);
    if (slot == 0)
        ++page_count;

    payload->snapshot_count = infs_cpu_to_le32(count + 1u);
    payload->reserved = infs_cpu_to_le32(page_count);
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)catalog;
    header->payload_size = infs_cpu_to_le32((uint32_t)(
        sizeof(*payload) + (size_t)page_count * sizeof(uint64_t)));

    if (!is_new)
        return write_object(vol, lookup.block, catalog);

    status = infs_object_finalize(catalog);
    if (status != INFS_STATUS_OK)
        return status;
    uint64_t object_block = 0, got = 0;
    status = alloc_run(vol, 1, &object_block, &got);
    if (status != INFS_STATUS_OK)
        return status;
    if (got != 1)
        return INFS_STATUS_CORRUPT;
    status = write_block(vol, object_block, catalog);
    if (status == INFS_STATUS_OK)
        status = index_add(vol, snapshot_catalog_id, object_block,
                           INFS_OBJECT_SNAPSHOT_CATALOG);
    if (status != INFS_STATUS_OK)
        free_run(vol, object_block, 1);
    return status;
}

infs_status infs_snapshot_create(struct infs_volume *vol, const char *name)
{
    size_t name_length = 0;
    if (!vol || !snapshot_name_valid(name, &name_length))
        return INFS_STATUS_INVALID_ARGUMENT;
    if (!snapshots_enabled(vol))
        return INFS_STATUS_NOT_SUPPORTED;
    if (!vol->writable)
        return INFS_STATUS_READ_ONLY;

    infs_status status = infs_volume_sync(vol);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_snapshot_record_disk existing;
    status = snapshot_find_record(vol, name, &existing);
    if (status == INFS_STATUS_OK)
        return INFS_STATUS_ALREADY_EXISTS;
    if (status != INFS_STATUS_NOT_FOUND)
        return status;

    status = tx_begin(vol);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_timestamp now = {0};
    status = infs_storage_current_time(&vol->storage, &now);
    if (status == INFS_STATUS_OK)
        status = snapshot_catalog_append(vol, name, name_length, &now);
    if (status != INFS_STATUS_OK)
        goto fail;

    /*
     * The snapshot record retains the already committed allocation-tree root.
     * Publication of this tiny catalogue mutation may replace allocation-tree
     * pages, but snapshot_union_bitmap() sees the new record before deferred
     * frees are applied and therefore preserves the complete retained tree.
     */
    status = finish_mutation(vol, (uint64_t)INFS_BLOCK_SIZE * 4u);
    if (status != INFS_STATUS_OK)
        goto fail;
    return INFS_STATUS_OK;

fail:
    tx_abort(vol);
    return status;
}

infs_status infs_snapshot_delete(struct infs_volume *vol, const char *name)
{
    size_t name_length = 0;
    if (!vol || !snapshot_name_valid(name, &name_length))
        return INFS_STATUS_INVALID_ARGUMENT;
    if (!snapshots_enabled(vol))
        return INFS_STATUS_NOT_SUPPORTED;
    if (!vol->writable)
        return INFS_STATUS_READ_ONLY;
    infs_status status = infs_volume_sync(vol);
    if (status != INFS_STATUS_OK)
        return status;
    status = tx_begin(vol);
    if (status != INFS_STATUS_OK)
        return status;

    uint8_t catalog[INFS_BLOCK_SIZE];
    struct infs_snapshot_catalog_payload_disk *payload;
    struct infs_lookup lookup;
    status = snapshot_catalog_load(vol, catalog, &payload, &lookup);
    if (status != INFS_STATUS_OK)
        goto fail;

    struct infs_snapshot_record_disk *records = NULL;
    uint32_t count = 0;
    status = snapshot_catalog_records(vol, catalog, payload, &records, &count);
    if (status != INFS_STATUS_OK)
        goto fail;
    uint32_t found = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (snapshot_record_name_equal(&records[i], name, name_length)) {
            found = i;
            break;
        }
    }
    if (found == count) {
        free(records);
        status = INFS_STATUS_NOT_FOUND;
        goto fail;
    }

    struct infs_snapshot_record_disk removed = records[found];
    free(records);
    uint8_t *removed_bitmap = NULL;
    status = snapshot_read_bitmap(vol, &removed, &removed_bitmap);
    if (status != INFS_STATUS_OK)
        goto fail;

    uint32_t last = count - 1u;
    uint32_t target_page_index = found / INFS_SNAPSHOT_RECORDS_PER_PAGE;
    uint32_t target_slot = found % INFS_SNAPSHOT_RECORDS_PER_PAGE;
    uint32_t last_page_index = last / INFS_SNAPSHOT_RECORDS_PER_PAGE;
    uint32_t last_slot = last % INFS_SNAPSHOT_RECORDS_PER_PAGE;
    uint32_t page_count = infs_le32_to_cpu(payload->reserved);
    uint64_t *pointers = (uint64_t *)(payload + 1);
    uint64_t catalog_generation =
        infs_le64_to_cpu(((struct infs_object_header_disk *)catalog)->generation);

    uint8_t target_page[INFS_BLOCK_SIZE];
    struct infs_snapshot_record_disk *target_records = NULL;
    uint32_t target_count = 0;
    uint64_t target_old = infs_le64_to_cpu(pointers[target_page_index]);
    status = snapshot_page_load(
        vol, target_old, catalog_generation,
        target_page, &target_records, &target_count);
    if (status != INFS_STATUS_OK) {
        free(removed_bitmap);
        goto fail;
    }

    if (target_page_index == last_page_index) {
        if (found != last)
            target_records[target_slot] = target_records[last_slot];
        memset(&target_records[last_slot], 0, sizeof(*target_records));
        struct infs_metadata_page_disk *page =
            (struct infs_metadata_page_disk *)target_page;
        if (target_count == 1u) {
            free_run(vol, target_old, 1u);
            pointers[last_page_index] = 0;
            --page_count;
        } else {
            page->entry_count = infs_cpu_to_le32(target_count - 1u);
            page->bytes_used = infs_cpu_to_le32(
                (target_count - 1u) * sizeof(*target_records));
            uint64_t replacement = 0;
            status = metadata_page_store(
                vol, target_old, target_page, &replacement);
            if (status != INFS_STATUS_OK) {
                free(removed_bitmap);
                goto fail;
            }
            pointers[target_page_index] = infs_cpu_to_le64(replacement);
        }
    } else {
        uint8_t last_page[INFS_BLOCK_SIZE];
        struct infs_snapshot_record_disk *last_records = NULL;
        uint32_t last_count = 0;
        uint64_t last_old = infs_le64_to_cpu(pointers[last_page_index]);
        status = snapshot_page_load(
            vol, last_old, catalog_generation,
            last_page, &last_records, &last_count);
        if (status != INFS_STATUS_OK || last_slot >= last_count) {
            free(removed_bitmap);
            status = status != INFS_STATUS_OK ? status : INFS_STATUS_CORRUPT;
            goto fail;
        }
        target_records[target_slot] = last_records[last_slot];
        uint64_t target_replacement = 0;
        status = metadata_page_store(
            vol, target_old, target_page, &target_replacement);
        if (status != INFS_STATUS_OK) {
            free(removed_bitmap);
            goto fail;
        }
        pointers[target_page_index] = infs_cpu_to_le64(target_replacement);

        memset(&last_records[last_slot], 0, sizeof(*last_records));
        struct infs_metadata_page_disk *last_meta =
            (struct infs_metadata_page_disk *)last_page;
        if (last_count == 1u) {
            free_run(vol, last_old, 1u);
            pointers[last_page_index] = 0;
            --page_count;
        } else {
            last_meta->entry_count = infs_cpu_to_le32(last_count - 1u);
            last_meta->bytes_used = infs_cpu_to_le32(
                (last_count - 1u) * sizeof(*last_records));
            uint64_t last_replacement = 0;
            status = metadata_page_store(
                vol, last_old, last_page, &last_replacement);
            if (status != INFS_STATUS_OK) {
                free(removed_bitmap);
                goto fail;
            }
            pointers[last_page_index] = infs_cpu_to_le64(last_replacement);
        }
    }

    payload->snapshot_count = infs_cpu_to_le32(count - 1u);
    payload->reserved = infs_cpu_to_le32(page_count);
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)catalog;
    header->payload_size = infs_cpu_to_le32((uint32_t)(
        sizeof(*payload) + (size_t)page_count * sizeof(uint64_t)));
    status = write_object(vol, lookup.block, catalog);
    if (status != INFS_STATUS_OK) {
        free(removed_bitmap);
        goto fail;
    }

    struct infs_index_entry_disk *entries = NULL;
    uint32_t entry_count = 0;
    status = index_snapshot(vol, &entries, &entry_count);
    uint8_t *live = calloc(1, vol->bitmap_bytes);
    uint8_t *retained = NULL;
    if (status == INFS_STATUS_OK && !live)
        status = INFS_STATUS_NO_MEMORY;
    if (status == INFS_STATUS_OK)
        status = build_live_ownership_bitmap(vol, entries, entry_count, live);
    if (status == INFS_STATUS_OK)
        status = snapshot_union_bitmap(vol, &retained);
    free(entries);
    if (status != INFS_STATUS_OK) {
        free(live);
        free(retained);
        free(removed_bitmap);
        goto fail;
    }

    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    uint64_t run_start = 0, run_count = 0;
    for (uint64_t b = 0; b <= total; ++b) {
        int reclaim = b < total && bitmap_get(removed_bitmap, b) &&
            !bitmap_get(live, b) && !bitmap_get(retained, b);
        if (reclaim) {
            if (!run_count)
                run_start = b;
            ++run_count;
        } else if (run_count) {
            status = tx_defer_range(vol, run_start, run_count);
            if (status != INFS_STATUS_OK)
                break;
            run_count = 0;
        }
    }
    free(live);
    free(retained);
    free(removed_bitmap);
    if (status != INFS_STATUS_OK)
        goto fail;
    status = finish_mutation(vol, vol->bitmap_bytes +
                             (uint64_t)INFS_BLOCK_SIZE * 4u);
    if (status != INFS_STATUS_OK)
        goto fail;
    return INFS_STATUS_OK;

fail:
    tx_abort(vol);
    return status;
}

infs_status infs_snapshot_rollback(struct infs_volume *vol,
                                      const char *name)
{
    if (!vol || !snapshot_name_valid(name, NULL))
        return INFS_STATUS_INVALID_ARGUMENT;
    if (!snapshots_enabled(vol))
        return INFS_STATUS_NOT_SUPPORTED;
    if (!vol->writable || vol->snapshot_view)
        return INFS_STATUS_READ_ONLY;
    if (vol->reopen_required_status != INFS_STATUS_OK)
        return vol->reopen_required_status;

    /*
     * Drain the live deferred transaction before selecting the retained graph.
     * The snapshot record then describes immutable, already-durable roots.
     */
    infs_status status = infs_volume_sync(vol);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_snapshot_record_disk record;
    status = snapshot_find_record(vol, name, &record);
    if (status != INFS_STATUS_OK)
        return status;

    /*
     * Do not make a retained generation authoritative merely because its
     * catalogue record is well-formed. Re-run the same graph validation used
     * for snapshot reads/scrub before publishing the rollback checkpoint.
     */
    struct infs_volume view;
    status = snapshot_open_view(vol, name, &view);
    if (status != INFS_STATUS_OK)
        return status;
    status = validate_index_entries(&view);
    if (status == INFS_STATUS_OK)
        status = validate_integrity_metadata(&view);
    if (status == INFS_STATUS_OK)
        status = validate_namespace_graph(&view);
    if (status == INFS_STATUS_OK)
        status = validate_checksum_graph(&view);
    snapshot_close_view(&view);
    if (status != INFS_STATUS_OK)
        return status;

    uint8_t *rollback_bitmap = NULL;
    struct infs_allocation_layout rollback_layout = {0};
    status = snapshot_load_allocation(
        vol, &record, &rollback_bitmap, &rollback_layout);
    if (status != INFS_STATUS_OK)
        return status;

    uint64_t generation = 0;
    if (!infiltratr_u64_add_checked(
            infs_le64_to_cpu(vol->sb.generation), UINT64_C(1),
            &generation) ||
        generation == 0) {
        allocation_map_layout_destroy(&rollback_layout);
        free(rollback_bitmap);
        return INFS_STATUS_OVERFLOW;
    }

    struct infs_superblock_disk next_sb = vol->sb;
    next_sb.generation = infs_cpu_to_le64(generation);
    next_sb.free_blocks = record.free_blocks;
    next_sb.allocation_root_block = record.allocation_root_block;
    next_sb.allocation_leaf_count = record.allocation_leaf_count;
    next_sb.object_index_block = record.object_index_block;
    next_sb.root_object_block = record.root_object_block;
    memcpy(next_sb.root_object_id, record.root_object_id,
           sizeof(next_sb.root_object_id));

    uint8_t sb_block[INFS_BLOCK_SIZE];
    status = infs_encode_superblock(sb_block, &next_sb);
    if (status != INFS_STATUS_OK)
        goto out;

    /*
     * Retained roots are already durable. Publishing block-zero first and
     * flushing it is therefore the rollback commit point. A crash before this
     * flush keeps the pre-rollback generation authoritative; a crash after it
     * selects the rollback generation and checkpoint healing can repair the
     * remaining replicas.
     */
    uint64_t primary = infs_le64_to_cpu(next_sb.checkpoint_block[0]);
    status = infs_storage_write(
        &vol->storage, primary * INFS_BLOCK_SIZE, sb_block, sizeof(sb_block));
    if (status != INFS_STATUS_OK)
        goto out;
    status = infs_storage_flush(&vol->storage);
    if (status != INFS_STATUS_OK) {
        vol->reopen_required_status = status;
        vol->writable = 0;
        goto out;
    }

    free(vol->bitmap);
    vol->bitmap = rollback_bitmap;
    rollback_bitmap = NULL;
    vol->sb = next_sb;
    allocation_map_cache_replace(vol, &rollback_layout);
    free_extent_index_destroy(vol);
    (void)free_extent_index_rebuild(vol);
    vol->data_allocation_cursor = 1u;
    vol->metadata_allocation_cursor =
        infs_le64_to_cpu(vol->sb.total_blocks);
    vol->checksum_cursor_valid = 0;
    object_cache_clear(vol);
    directory_cache_clear(vol);
    (void)object_cache_rebuild(vol);

    infs_status replica_status = INFS_STATUS_OK;
    for (unsigned i = 1; i < INFS_CHECKPOINT_COUNT; ++i) {
        uint64_t checkpoint =
            infs_le64_to_cpu(next_sb.checkpoint_block[i]);
        infs_status one = infs_storage_write(
            &vol->storage, checkpoint * INFS_BLOCK_SIZE,
            sb_block, sizeof(sb_block));
        if (one != INFS_STATUS_OK && replica_status == INFS_STATUS_OK)
            replica_status = one;
    }
    infs_status flush_status = infs_storage_flush(&vol->storage);
    if (flush_status != INFS_STATUS_OK &&
        replica_status == INFS_STATUS_OK)
        replica_status = flush_status;
    vol->checkpoint_repair_needed =
        replica_status != INFS_STATUS_OK;
    status = INFS_STATUS_OK;

out:
    allocation_map_layout_destroy(&rollback_layout);
    free(rollback_bitmap);
    return status;
}

infs_status infs_snapshot_list(struct infs_volume *vol,
                               struct infs_snapshot_info **snapshots,
                               size_t *count_out)
{
    if (!vol || !snapshots || !count_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    *snapshots = NULL;
    *count_out = 0;
    if (!snapshots_enabled(vol))
        return INFS_STATUS_NOT_SUPPORTED;

    uint8_t catalog[INFS_BLOCK_SIZE];
    struct infs_snapshot_catalog_payload_disk *payload;
    infs_status status = snapshot_catalog_load(vol, catalog, &payload, NULL);
    if (status == INFS_STATUS_NOT_FOUND)
        return INFS_STATUS_OK;
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_snapshot_record_disk *records = NULL;
    uint32_t count = 0;
    status = snapshot_catalog_records(vol, catalog, payload, &records, &count);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_snapshot_info *list =
        calloc(count ? count : 1u, sizeof(*list));
    if (!list) {
        free(records);
        return INFS_STATUS_NO_MEMORY;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t length = infs_le16_to_cpu(records[i].name_length);
        memcpy(list[i].name, records[i].name, length);
        list[i].name[length] = '\0';
        list[i].generation = infs_le64_to_cpu(records[i].generation);
        timestamp_value_from_disk(
            &records[i].created_time, &list[i].created_time);
    }
    free(records);
    *snapshots = list;
    *count_out = count;
    return INFS_STATUS_OK;
}

void infs_free_snapshot_infos(struct infs_snapshot_info *snapshots)
{
    free(snapshots);
}

infs_status infs_snapshot_lookup_path(struct infs_volume *vol,
                                      const char *snapshot, const char *path,
                                      struct infs_lookup *out)
{
    if (!path || !out)
        return INFS_STATUS_INVALID_ARGUMENT;
    struct infs_volume view;
    infs_status status = snapshot_open_view(vol, snapshot, &view);
    if (status == INFS_STATUS_OK) {
        status = infs_lookup_path(&view, path, out);
        snapshot_close_view(&view);
    }
    return status;
}

infs_status infs_snapshot_get_attributes(struct infs_volume *vol,
                                          const char *snapshot,
                                          const char *path,
                                          struct infs_attributes *attributes)
{
    struct infs_volume view;
    infs_status status = snapshot_open_view(vol, snapshot, &view);
    if (status == INFS_STATUS_OK) {
        status = infs_get_attributes(&view, path, attributes);
        snapshot_close_view(&view);
    }
    return status;
}

infs_status infs_snapshot_list_dir(struct infs_volume *vol,
                                   const char *snapshot, const char *path,
                                   struct infs_dir_item **items,
                                   size_t *count)
{
    struct infs_volume view;
    infs_status status = snapshot_open_view(vol, snapshot, &view);
    if (status == INFS_STATUS_OK) {
        status = infs_list_dir(&view, path, items, count);
        snapshot_close_view(&view);
    }
    return status;
}

int64_t infs_snapshot_read_file(struct infs_volume *vol,
                                const char *snapshot, const char *path,
                                void *buf, size_t size, uint64_t offset)
{
    struct infs_volume view;
    infs_status status = snapshot_open_view(vol, snapshot, &view);
    if (status != INFS_STATUS_OK)
        return status;
    int64_t result = infs_read_file(&view, path, buf, size, offset);
    snapshot_close_view(&view);
    return result;
}

infs_status infs_snapshot_read_symlink(struct infs_volume *vol,
                                       const char *snapshot,
                                       const char *path, char *target,
                                       size_t capacity, size_t *length_out)
{
    struct infs_volume view;
    infs_status status = snapshot_open_view(vol, snapshot, &view);
    if (status == INFS_STATUS_OK) {
        status = infs_read_symlink(&view, path, target, capacity, length_out);
        snapshot_close_view(&view);
    }
    return status;
}


struct infs_snapshot_restore_link {
    uint8_t object_id[16];
    char *destination_path;
};

struct infs_snapshot_restore_context {
    struct infs_volume *live;
    struct infs_volume *snapshot;
    struct infs_snapshot_restore_link *links;
    size_t link_count;
    size_t link_capacity;
};

static int snapshot_restore_id_is_zero(const uint8_t id[16])
{
    uint8_t any = 0;
    for (size_t i = 0; i < 16u; ++i)
        any |= id[i];
    return any == 0;
}

static infs_status snapshot_restore_join_path(
    const char *base, const char *name, char **out)
{
    if (!base || !name || !out)
        return INFS_STATUS_INVALID_ARGUMENT;

    size_t base_len = strlen(base);
    size_t name_len = strlen(name);
    if (!name_len || base_len == 0 || base[0] != '/')
        return INFS_STATUS_INVALID_ARGUMENT;

    size_t slash = strcmp(base, "/") == 0 ? 0u : 1u;
    if (base_len > INFS_PATH_MAX || name_len > INFS_NAME_MAX ||
        slash > SIZE_MAX - base_len ||
        name_len > SIZE_MAX - base_len - slash - 1u)
        return INFS_STATUS_NAME_TOO_LONG;

    size_t total = base_len + slash + name_len;
    if (total >= INFS_PATH_MAX)
        return INFS_STATUS_NAME_TOO_LONG;

    char *path = malloc(total + 1u);
    if (!path)
        return INFS_STATUS_NO_MEMORY;
    memcpy(path, base, base_len);
    size_t at = base_len;
    if (slash)
        path[at++] = '/';
    memcpy(path + at, name, name_len);
    path[total] = '\0';
    *out = path;
    return INFS_STATUS_OK;
}

static infs_status snapshot_restore_remove_live_tree(
    struct infs_volume *live, const char *path)
{
    struct infs_lookup lookup;
    infs_status status = infs_lookup_path(live, path, &lookup);
    if (status == INFS_STATUS_NOT_FOUND)
        return INFS_STATUS_OK;
    if (status != INFS_STATUS_OK)
        return status;
    if (strcmp(path, "/") == 0)
        return INFS_STATUS_INVALID_ARGUMENT;

    if (lookup.type != INFS_OBJECT_DIRECTORY)
        return infs_unlink(live, path);

    struct infs_dir_item *items = NULL;
    size_t count = 0;
    status = infs_list_dir(live, path, &items, &count);
    if (status != INFS_STATUS_OK)
        return status;

    for (size_t i = 0; i < count; ++i) {
        char *child = NULL;
        status = snapshot_restore_join_path(path, items[i].name, &child);
        if (status == INFS_STATUS_OK)
            status = snapshot_restore_remove_live_tree(live, child);
        free(child);
        if (status != INFS_STATUS_OK)
            break;
    }
    infs_free_dir_items(items);
    if (status != INFS_STATUS_OK)
        return status;
    return infs_rmdir(live, path);
}

static infs_status snapshot_restore_link_remember(
    struct infs_snapshot_restore_context *ctx,
    const uint8_t object_id[16], const char *destination_path)
{
    if (ctx->link_count == ctx->link_capacity) {
        size_t next = ctx->link_capacity ? ctx->link_capacity * 2u : 16u;
        if (next < ctx->link_capacity ||
            next > SIZE_MAX / sizeof(*ctx->links))
            return INFS_STATUS_OVERFLOW;
        struct infs_snapshot_restore_link *grown =
            realloc(ctx->links, next * sizeof(*grown));
        if (!grown)
            return INFS_STATUS_NO_MEMORY;
        ctx->links = grown;
        ctx->link_capacity = next;
    }

    char *copy = malloc(strlen(destination_path) + 1u);
    if (!copy)
        return INFS_STATUS_NO_MEMORY;
    strcpy(copy, destination_path);
    memcpy(ctx->links[ctx->link_count].object_id, object_id, 16);
    ctx->links[ctx->link_count].destination_path = copy;
    ctx->link_count++;
    return INFS_STATUS_OK;
}

static const char *snapshot_restore_link_find(
    const struct infs_snapshot_restore_context *ctx,
    const uint8_t object_id[16])
{
    for (size_t i = 0; i < ctx->link_count; ++i)
        if (memcmp(ctx->links[i].object_id, object_id, 16) == 0)
            return ctx->links[i].destination_path;
    return NULL;
}

static infs_status snapshot_restore_ensure_principal(
    struct infs_snapshot_restore_context *ctx, const uint8_t principal_id[16])
{
    if (snapshot_restore_id_is_zero(principal_id) ||
        infs_security_principal_id_is_reserved(principal_id))
        return INFS_STATUS_OK;

    struct infs_security_principal principal = {0};
    infs_status status = infs_get_security_principal(
        ctx->live, principal_id, &principal);
    if (status == INFS_STATUS_OK) {
        infs_free_security_principal(&principal);
        return INFS_STATUS_OK;
    }
    if (status != INFS_STATUS_NOT_FOUND)
        return status;

    status = infs_get_security_principal(
        ctx->snapshot, principal_id, &principal);
    if (status != INFS_STATUS_OK)
        return status;
    status = infs_put_security_principal(ctx->live, &principal);
    infs_free_security_principal(&principal);
    return status;
}

static infs_status snapshot_restore_security(
    struct infs_snapshot_restore_context *ctx,
    const char *source_path, const char *destination_path)
{
    struct infs_security_descriptor descriptor = {0};
    infs_status status = infs_get_security_descriptor(
        ctx->snapshot, source_path, &descriptor);
    if (status == INFS_STATUS_NOT_FOUND)
        return INFS_STATUS_OK;
    if (status != INFS_STATUS_OK)
        return status;

    status = snapshot_restore_ensure_principal(
        ctx, descriptor.owner_principal_id);
    if (status == INFS_STATUS_OK)
        status = snapshot_restore_ensure_principal(
            ctx, descriptor.primary_group_principal_id);
    for (size_t i = 0; status == INFS_STATUS_OK &&
                         i < descriptor.ace_count; ++i)
        status = snapshot_restore_ensure_principal(
            ctx, descriptor.aces[i].principal_id);
    if (status == INFS_STATUS_OK)
        status = infs_set_security_descriptor(
            ctx->live, destination_path, &descriptor);
    infs_free_security_descriptor(&descriptor);
    return status;
}

static infs_status snapshot_restore_named_metadata(
    struct infs_snapshot_restore_context *ctx,
    const char *source_path, const char *destination_path)
{
    struct infs_typed_extension extension = {0};
    infs_status status = infs_get_typed_extension(
        ctx->snapshot, source_path, &extension);
    if (status == INFS_STATUS_OK) {
        status = infs_set_typed_extension(
            ctx->live, destination_path, &extension);
        infs_free_typed_extension(&extension);
        if (status != INFS_STATUS_OK)
            return status;
    } else if (status != INFS_STATUS_NOT_FOUND) {
        return status;
    }

    struct infs_named_stream_info *streams = NULL;
    size_t stream_count = 0;
    status = infs_named_stream_list(
        ctx->snapshot, source_path, &streams, &stream_count);
    if (status == INFS_STATUS_NOT_FOUND ||
        status == INFS_STATUS_NOT_SUPPORTED)
        return INFS_STATUS_OK;
    if (status != INFS_STATUS_OK)
        return status;

    for (size_t i = 0; i < stream_count; ++i) {
        if (streams[i].logical_size > SIZE_MAX) {
            status = INFS_STATUS_FILE_TOO_LARGE;
            break;
        }
        size_t size = (size_t)streams[i].logical_size;
        uint8_t *data = malloc(size ? size : 1u);
        if (!data) {
            status = INFS_STATUS_NO_MEMORY;
            break;
        }
        size_t done = 0;
        while (done < size) {
            size_t chunk = size - done;
            if (chunk > 256u * 1024u)
                chunk = 256u * 1024u;
            int64_t got = infs_named_stream_read(
                ctx->snapshot, source_path, streams[i].name,
                data + done, chunk, done);
            if (got <= 0 || (size_t)got > chunk) {
                status = got < 0 ? (infs_status)got : INFS_STATUS_IO_ERROR;
                break;
            }
            done += (size_t)got;
        }
        if (status == INFS_STATUS_OK)
            status = infs_named_stream_set(
                ctx->live, destination_path, streams[i].name, data, size);
        free(data);
        if (status != INFS_STATUS_OK)
            break;
    }
    infs_free_named_stream_infos(streams);
    return status;
}

static infs_status snapshot_restore_apply_metadata(
    struct infs_snapshot_restore_context *ctx,
    const char *source_path, const char *destination_path,
    const struct infs_attributes *attributes)
{
    infs_status status = snapshot_restore_security(
        ctx, source_path, destination_path);
    if (status == INFS_STATUS_OK)
        status = snapshot_restore_named_metadata(
            ctx, source_path, destination_path);
    if (status == INFS_STATUS_OK)
        status = infs_set_portable_flags(
            ctx->live, destination_path, attributes->portable_flags);
    if (status == INFS_STATUS_OK)
        status = infs_set_posix_compat(
            ctx->live, destination_path,
            INFS_POSIX_SET_PERMISSIONS | INFS_POSIX_SET_UID |
                INFS_POSIX_SET_GID,
            attributes->posix_permissions, attributes->posix_uid,
            attributes->posix_gid);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_time_update times = {
        .birth_action = INFS_TIME_SET,
        .access_action = INFS_TIME_SET,
        .modification_action = INFS_TIME_SET,
        .change_action = INFS_TIME_SET,
        .birth_time = attributes->birth_time,
        .access_time = attributes->access_time,
        .modification_time = attributes->modification_time,
        .change_time = attributes->change_time,
    };
    return infs_set_times(ctx->live, destination_path, &times);
}

static infs_status snapshot_restore_regular_file(
    struct infs_snapshot_restore_context *ctx,
    const char *source_path, const char *destination_path,
    const struct infs_attributes *attributes)
{
    const char *existing = snapshot_restore_link_find(
        ctx, attributes->object_id);
    if (existing)
        return infs_link_file(ctx->live, existing, destination_path);

    struct infs_create_options options = {
        .portable_flags = attributes->portable_flags,
        .posix_permissions = attributes->posix_permissions,
        .posix_uid = attributes->posix_uid,
        .posix_gid = attributes->posix_gid,
    };
    infs_status status = infs_create_file(
        ctx->live, destination_path, &options);
    if (status != INFS_STATUS_OK)
        return status;

    uint8_t *buffer = malloc(256u * 1024u);
    if (!buffer)
        return INFS_STATUS_NO_MEMORY;
    uint64_t offset = 0;
    while (offset < attributes->logical_size) {
        size_t want = 256u * 1024u;
        if ((uint64_t)want > attributes->logical_size - offset)
            want = (size_t)(attributes->logical_size - offset);
        int64_t got = infs_read_file(
            ctx->snapshot, source_path, buffer, want, offset);
        if (got <= 0 || (size_t)got > want) {
            status = got < 0 ? (infs_status)got : INFS_STATUS_IO_ERROR;
            break;
        }
        int64_t written = infs_write_file_buffered(
            ctx->live, destination_path, buffer, (size_t)got, offset);
        if (written != got) {
            status = written < 0 ? (infs_status)written :
                                   INFS_STATUS_IO_ERROR;
            break;
        }
        offset += (uint64_t)got;
    }
    free(buffer);
    if (status != INFS_STATUS_OK)
        return status;

    if (attributes->logical_size == 0)
        status = infs_truncate_file(ctx->live, destination_path, 0);
    if (status == INFS_STATUS_OK)
        status = snapshot_restore_apply_metadata(
            ctx, source_path, destination_path, attributes);
    if (status == INFS_STATUS_OK)
        status = snapshot_restore_link_remember(
            ctx, attributes->object_id, destination_path);
    return status;
}

static infs_status snapshot_restore_one(
    struct infs_snapshot_restore_context *ctx,
    const char *source_path, const char *destination_path)
{
    struct infs_attributes attributes;
    infs_status status = infs_get_attributes(
        ctx->snapshot, source_path, &attributes);
    if (status != INFS_STATUS_OK)
        return status;

    if (attributes.object_type == INFS_OBJECT_FILE)
        return snapshot_restore_regular_file(
            ctx, source_path, destination_path, &attributes);

    struct infs_create_options options = {
        .portable_flags = attributes.portable_flags,
        .posix_permissions = attributes.posix_permissions,
        .posix_uid = attributes.posix_uid,
        .posix_gid = attributes.posix_gid,
    };

    if (attributes.object_type == INFS_OBJECT_SYMLINK) {
        char target[INFS_SYMLINK_TARGET_MAX + 1u];
        size_t target_length = 0;
        status = infs_read_symlink(
            ctx->snapshot, source_path, target, sizeof(target),
            &target_length);
        if (status != INFS_STATUS_OK)
            return status;
        status = infs_create_symlink(
            ctx->live, destination_path, target, &options);
        if (status == INFS_STATUS_OK)
            status = snapshot_restore_apply_metadata(
                ctx, source_path, destination_path, &attributes);
        return status;
    }

    if (attributes.object_type != INFS_OBJECT_DIRECTORY)
        return INFS_STATUS_NOT_SUPPORTED;

    status = infs_mkdir(ctx->live, destination_path, &options);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_dir_item *items = NULL;
    size_t count = 0;
    status = infs_list_dir(ctx->snapshot, source_path, &items, &count);
    if (status != INFS_STATUS_OK)
        return status;
    for (size_t i = 0; i < count; ++i) {
        char *source_child = NULL;
        char *destination_child = NULL;
        status = snapshot_restore_join_path(
            source_path, items[i].name, &source_child);
        if (status == INFS_STATUS_OK)
            status = snapshot_restore_join_path(
                destination_path, items[i].name, &destination_child);
        if (status == INFS_STATUS_OK)
            status = snapshot_restore_one(
                ctx, source_child, destination_child);
        free(source_child);
        free(destination_child);
        if (status != INFS_STATUS_OK)
            break;
    }
    infs_free_dir_items(items);
    if (status == INFS_STATUS_OK)
        status = snapshot_restore_apply_metadata(
            ctx, source_path, destination_path, &attributes);
    return status;
}

infs_status infs_snapshot_restore_path(
    struct infs_volume *vol, const char *snapshot,
    const char *source_path, const char *destination_path, int replace)
{
    if (!vol || !snapshot || !source_path || !destination_path ||
        source_path[0] != '/' || destination_path[0] != '/' ||
        strcmp(destination_path, "/") == 0)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (!snapshots_enabled(vol))
        return INFS_STATUS_NOT_SUPPORTED;
    if (!vol->writable || vol->snapshot_view)
        return INFS_STATUS_READ_ONLY;
    if (vol->reopen_required_status != INFS_STATUS_OK)
        return vol->reopen_required_status;

    infs_status status = infs_volume_sync(vol);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_volume view;
    status = snapshot_open_view(vol, snapshot, &view);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_lookup source_lookup;
    status = infs_lookup_path(&view, source_path, &source_lookup);
    if (status != INFS_STATUS_OK) {
        snapshot_close_view(&view);
        return status;
    }

    struct infs_lookup destination_lookup;
    infs_status destination_status =
        infs_lookup_path(vol, destination_path, &destination_lookup);
    if (destination_status == INFS_STATUS_OK && !replace) {
        snapshot_close_view(&view);
        return INFS_STATUS_ALREADY_EXISTS;
    }
    if (destination_status != INFS_STATUS_OK &&
        destination_status != INFS_STATUS_NOT_FOUND) {
        snapshot_close_view(&view);
        return destination_status;
    }

    int old_deferred = vol->deferred_publish;
    uint64_t old_threshold = vol->deferred_publish_threshold_bytes;
    vol->deferred_publish = 1;
    vol->deferred_publish_threshold_bytes = UINT64_MAX;

    if (destination_status == INFS_STATUS_OK)
        status = snapshot_restore_remove_live_tree(vol, destination_path);

    struct infs_snapshot_restore_context ctx = {
        .live = vol,
        .snapshot = &view,
    };
    if (status == INFS_STATUS_OK)
        status = snapshot_restore_one(
            &ctx, source_path, destination_path);
    if (status == INFS_STATUS_OK)
        status = infs_volume_sync(vol);
    else if (vol->tx_active)
        tx_abort(vol);

    vol->deferred_publish = old_deferred;
    vol->deferred_publish_threshold_bytes = old_threshold;

    for (size_t i = 0; i < ctx.link_count; ++i)
        free(ctx.links[i].destination_path);
    free(ctx.links);
    snapshot_close_view(&view);
    return status;
}
