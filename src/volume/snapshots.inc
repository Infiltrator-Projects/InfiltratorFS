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
    if (ctx->count == ctx->capacity) {
        size_t next = ctx->capacity ? ctx->capacity * 2u : 32u;
        if (next < ctx->capacity ||
            next > SIZE_MAX / sizeof(*ctx->entries))
            return INFS_STATUS_NO_MEMORY;
        struct snapshot_visit_entry *grown = realloc(
            ctx->entries, next * sizeof(*grown));
        if (!grown)
            return INFS_STATUS_NO_MEMORY;
        ctx->entries = grown;
        ctx->capacity = next;
    }
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
    uint64_t bitmap_start = infs_le64_to_cpu(record->bitmap_start_block);
    uint64_t bitmap_blocks = infs_le64_to_cpu(record->bitmap_block_count);
    uint64_t expected_bitmap_blocks =
        (uint64_t)(vol->bitmap_bytes / INFS_BLOCK_SIZE);
    uint64_t index = infs_le64_to_cpu(record->object_index_block);
    uint64_t root = infs_le64_to_cpu(record->root_object_block);
    uint16_t name_length = infs_le16_to_cpu(record->name_length);

    if (catalog_generation == 0 || generation == 0 ||
        generation >= catalog_generation ||
        bitmap_blocks == 0 || bitmap_blocks != expected_bitmap_blocks ||
        bitmap_start == 0 || bitmap_start >= total ||
        bitmap_blocks > total - bitmap_start || index == 0 || index >= total ||
        root == 0 || root >= total ||
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
        memcmp(header->object_id, snapshot_catalog_id, 16) != 0 ||
        !id_is_zero(header->parent_id))
        return INFS_STATUS_CORRUPT;
    struct infs_snapshot_catalog_payload_disk *payload =
        (struct infs_snapshot_catalog_payload_disk *)(header + 1);
    uint32_t count = infs_le32_to_cpu(payload->snapshot_count);
    if (count > INFS_SNAPSHOTS_PER_CATALOG ||
        infs_le32_to_cpu(payload->reserved) != 0)
        return INFS_STATUS_CORRUPT;
    struct infs_snapshot_record_disk *records =
        (struct infs_snapshot_record_disk *)(payload + 1);
    uint64_t catalog_generation = infs_le64_to_cpu(header->generation);
    for (uint32_t i = 0; i < count; ++i) {
        status = snapshot_record_valid(
            vol, &records[i], catalog_generation);
        if (status != INFS_STATUS_OK)
            return status;
        for (uint32_t j = 0; j < i; ++j) {
            uint16_t a = infs_le16_to_cpu(records[i].name_length);
            uint16_t b = infs_le16_to_cpu(records[j].name_length);
            if (a == b && memcmp(records[i].name, records[j].name, a) == 0)
                return INFS_STATUS_CORRUPT;
        }
    }
    if (payload_out)
        *payload_out = payload;
    if (lookup_out)
        *lookup_out = lookup;
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
    uint32_t count = infs_le32_to_cpu(payload->snapshot_count);
    struct infs_snapshot_record_disk *records =
        (struct infs_snapshot_record_disk *)(payload + 1);
    for (uint32_t i = 0; i < count; ++i) {
        if (snapshot_record_name_equal(&records[i], name, length)) {
            if (record_out)
                *record_out = records[i];
            return INFS_STATUS_OK;
        }
    }
    return INFS_STATUS_NOT_FOUND;
}

static infs_status snapshot_read_bitmap(
    struct infs_volume *vol, const struct infs_snapshot_record_disk *record,
    uint8_t **bitmap_out)
{
    if (!bitmap_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    *bitmap_out = NULL;
    uint8_t *bitmap = malloc(vol->bitmap_bytes);
    if (!bitmap)
        return INFS_STATUS_NO_MEMORY;
    uint64_t start = infs_le64_to_cpu(record->bitmap_start_block);
    infs_status status = infs_storage_read(
        &vol->storage, start * INFS_BLOCK_SIZE, bitmap, vol->bitmap_bytes);
    if (status != INFS_STATUS_OK) {
        free(bitmap);
        return status;
    }
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    uint64_t free_count = 0;
    for (uint64_t b = 0; b < total; ++b) {
        if (!bitmap_get(bitmap, b))
            ++free_count;
    }
    if (free_count != infs_le64_to_cpu(record->free_blocks)) {
        free(bitmap);
        return INFS_STATUS_CORRUPT;
    }
    for (uint64_t b = total; b < (uint64_t)vol->bitmap_bytes * 8u; ++b) {
        if (!bitmap_get(bitmap, b)) {
            free(bitmap);
            return INFS_STATUS_CORRUPT;
        }
    }
    uint64_t blocks = infs_le64_to_cpu(record->bitmap_block_count);
    for (uint64_t i = 0; i < blocks; ++i) {
        if (!bitmap_get(vol->bitmap, start + i) ||
            !bitmap_get(bitmap, start + i)) {
            free(bitmap);
            return INFS_STATUS_CORRUPT;
        }
    }
    *bitmap_out = bitmap;
    return INFS_STATUS_OK;
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
    uint32_t count = infs_le32_to_cpu(payload->snapshot_count);
    struct infs_snapshot_record_disk *records =
        (struct infs_snapshot_record_disk *)(payload + 1);
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t *bitmap = NULL;
        status = snapshot_read_bitmap(vol, &records[i], &bitmap);
        if (status != INFS_STATUS_OK) {
            free(*union_out);
            *union_out = NULL;
            return status;
        }
        for (size_t j = 0; j < vol->bitmap_bytes; ++j)
            (*union_out)[j] |= bitmap[j];
        free(bitmap);
    }
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
    status = snapshot_read_bitmap(vol, &record, &bitmap);
    if (status != INFS_STATUS_OK)
        return status;

    memset(view, 0, sizeof(*view));
    view->storage = vol->storage;
    view->size_bytes = vol->size_bytes;
    view->sb = vol->sb;
    view->bitmap = bitmap;
    view->bitmap_bytes = vol->bitmap_bytes;
    view->data_allocation_cursor = 1u;
    view->metadata_allocation_cursor =
        infs_le64_to_cpu(view->sb.total_blocks);
    view->snapshot_validation_context = vol->snapshot_validation_context;
    view->snapshot_scrub_context = vol->snapshot_scrub_context;
    view->snapshot_view = 1;
    view->snapshot_bitmap_start_block =
        infs_le64_to_cpu(record.bitmap_start_block);
    view->snapshot_bitmap_block_count =
        infs_le64_to_cpu(record.bitmap_block_count);
    view->sb.generation = record.generation;
    view->sb.free_blocks = record.free_blocks;
    view->sb.object_index_block = record.object_index_block;
    view->sb.root_object_block = record.root_object_block;
    memcpy(view->sb.root_object_id, record.root_object_id, 16);
    return INFS_STATUS_OK;
}

static void snapshot_close_view(struct infs_volume *view)
{
    if (!view)
        return;
    object_cache_destroy(view);
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
    uint32_t count = infs_le32_to_cpu(payload->snapshot_count);
    struct infs_snapshot_record_disk *records =
        (struct infs_snapshot_record_disk *)(payload + 1);
    for (uint32_t i = 0; i < count; ++i) {
        int first_visit = 0;
        status = snapshot_visit_once(visit, &records[i], &first_visit);
        if (status != INFS_STATUS_OK)
            goto out;
        if (!first_visit)
            continue;
        char name[INFS_SNAPSHOT_NAME_MAX + 1u] = {0};
        uint16_t length = infs_le16_to_cpu(records[i].name_length);
        memcpy(name, records[i].name, length);
        struct infs_volume view;
        status = snapshot_open_view(vol, name, &view);
        if (status != INFS_STATUS_OK)
            goto out;
        status = validate_index_entries(&view);
        if (status == INFS_STATUS_OK)
            status = validate_integrity_metadata(&view);
        if (status == INFS_STATUS_OK)
            status = validate_namespace_graph(&view);
        if (status == INFS_STATUS_OK)
            status = validate_checksum_graph(&view);
        snapshot_close_view(&view);
        if (status != INFS_STATUS_OK)
            goto out;
    }
out:
    if (owns_context) {
        vol->snapshot_validation_context = NULL;
        snapshot_visit_context_destroy(visit);
    }
    return status;
}

static int snapshot_catalog_append(
    struct infs_volume *vol, const char *name, size_t name_length,
    uint64_t bitmap_start, const uint8_t *snapshot_bitmap,
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
        payload = (struct infs_snapshot_catalog_payload_disk *)(
            catalog + sizeof(struct infs_object_header_disk));
        memset(payload, 0, sizeof(*payload));
    }

    uint32_t count = infs_le32_to_cpu(payload->snapshot_count);
    if (count >= INFS_SNAPSHOTS_PER_CATALOG)
        return INFS_STATUS_NO_SPACE;
    struct infs_snapshot_record_disk *records =
        (struct infs_snapshot_record_disk *)(payload + 1);
    for (uint32_t i = 0; i < count; ++i) {
        if (snapshot_record_name_equal(&records[i], name, name_length))
            return INFS_STATUS_ALREADY_EXISTS;
    }

    struct infs_snapshot_record_disk *record = &records[count];
    memset(record, 0, sizeof(*record));
    record->generation = vol->tx_base_sb.generation;
    timestamp_disk_from_value(&record->created_time, created_time);
    record->bitmap_start_block = infs_cpu_to_le64(bitmap_start);
    record->bitmap_block_count = infs_cpu_to_le64(
        (uint64_t)(vol->bitmap_bytes / INFS_BLOCK_SIZE));
    uint64_t free_count = 0;
    uint64_t total = infs_le64_to_cpu(vol->tx_base_sb.total_blocks);
    for (uint64_t b = 0; b < total; ++b) {
        if (!bitmap_get(snapshot_bitmap, b))
            ++free_count;
    }
    record->free_blocks = infs_cpu_to_le64(free_count);
    record->object_index_block = vol->tx_base_sb.object_index_block;
    record->root_object_block = vol->tx_base_sb.root_object_block;
    memcpy(record->root_object_id, vol->tx_base_sb.root_object_id, 16);
    record->name_length = infs_cpu_to_le16((uint16_t)name_length);
    memcpy(record->name, name, name_length);
    payload->snapshot_count = infs_cpu_to_le32(count + 1u);
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)catalog;
    header->payload_size = infs_cpu_to_le32((uint32_t)(sizeof(*payload) +
        (size_t)(count + 1u) * sizeof(*record)));

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

    uint8_t *snapshot_bitmap = malloc(vol->bitmap_bytes);
    if (!snapshot_bitmap) {
        status = INFS_STATUS_NO_MEMORY;
        goto fail;
    }
    memcpy(snapshot_bitmap, vol->bitmap, vol->bitmap_bytes);

    struct infs_allocation_layout live_map = {0};
    status = allocation_map_cache_view(vol, &vol->tx_base_sb, &live_map);
    if (status != INFS_STATUS_OK) {
        free(snapshot_bitmap);
        goto fail;
    }
    for (size_t i = 0; i < live_map.leaf_count; ++i)
        bitmap_set(snapshot_bitmap, live_map.leaf_blocks[i], 0);
    for (size_t i = 0; i < live_map.branch_count; ++i)
        bitmap_set(snapshot_bitmap, live_map.branch_blocks[i], 0);

    uint64_t bitmap_blocks =
        (uint64_t)(vol->bitmap_bytes / INFS_BLOCK_SIZE);
    uint64_t bitmap_start = 0;
    status = alloc_exact_run(vol, bitmap_blocks, &bitmap_start);
    if (status != INFS_STATUS_OK) {
        free(snapshot_bitmap);
        goto fail;
    }
    for (uint64_t i = 0; i < bitmap_blocks; ++i)
        bitmap_set(snapshot_bitmap, bitmap_start + i, 1);
    status = infs_storage_write(&vol->storage,
        bitmap_start * INFS_BLOCK_SIZE, snapshot_bitmap, vol->bitmap_bytes);
    if (status != INFS_STATUS_OK) {
        free(snapshot_bitmap);
        goto fail;
    }
    struct infs_timestamp now = {0};
    status = infs_storage_current_time(&vol->storage, &now);
    if (status == INFS_STATUS_OK)
        status = snapshot_catalog_append(vol, name, name_length, bitmap_start,
                                         snapshot_bitmap, &now);
    free(snapshot_bitmap);
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
    uint32_t count = infs_le32_to_cpu(payload->snapshot_count);
    struct infs_snapshot_record_disk *records =
        (struct infs_snapshot_record_disk *)(payload + 1);
    uint32_t found = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (snapshot_record_name_equal(&records[i], name, name_length)) {
            found = i;
            break;
        }
    }
    if (found == count) {
        status = INFS_STATUS_NOT_FOUND;
        goto fail;
    }
    struct infs_snapshot_record_disk removed = records[found];
    uint8_t *removed_bitmap = NULL;
    status = snapshot_read_bitmap(vol, &removed, &removed_bitmap);
    if (status != INFS_STATUS_OK)
        goto fail;

    memmove(&records[found], &records[found + 1u],
            (size_t)(count - found - 1u) * sizeof(*records));
    memset(&records[count - 1u], 0, sizeof(*records));
    payload->snapshot_count = infs_cpu_to_le32(count - 1u);
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)catalog;
    header->payload_size = infs_cpu_to_le32((uint32_t)(sizeof(*payload) +
        (size_t)(count - 1u) * sizeof(*records)));
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
                             (uint64_t)INFS_BLOCK_SIZE * 3u);
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
    uint32_t count = infs_le32_to_cpu(payload->snapshot_count);
    struct infs_snapshot_info *list = calloc(count ? count : 1u, sizeof(*list));
    if (!list)
        return INFS_STATUS_NO_MEMORY;
    struct infs_snapshot_record_disk *records =
        (struct infs_snapshot_record_disk *)(payload + 1);
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t length = infs_le16_to_cpu(records[i].name_length);
        memcpy(list[i].name, records[i].name, length);
        list[i].name[length] = '\0';
        list[i].generation = infs_le64_to_cpu(records[i].generation);
        timestamp_value_from_disk(&records[i].created_time, &list[i].created_time);
    }
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
