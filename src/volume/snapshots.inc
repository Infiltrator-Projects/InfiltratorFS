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
        le32_to_cpu(header->payload_size) !=
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
