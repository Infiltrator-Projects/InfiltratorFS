// SPDX-License-Identifier: GPL-3.0-or-later
static int scrub_reportable_metadata_status(infs_status status)
{
    return status == INFS_STATUS_CORRUPT ||
           status == INFS_STATUS_NOT_FOUND ||
           status == INFS_STATUS_LOOP_DETECTED;
}

static infs_status scrub_validate_full_metadata(
    struct infs_volume *vol, struct infs_scrub_report *report)
{
    infs_status status = validate_index_entries(vol);
    if (status == INFS_STATUS_OK)
        status = validate_integrity_metadata(vol);
    if (status == INFS_STATUS_OK)
        status = validate_namespace_graph(vol);
    if (status == INFS_STATUS_OK)
        status = validate_checksum_graph(vol);

    if (status == INFS_STATUS_OK)
        return INFS_STATUS_OK;
    if (scrub_reportable_metadata_status(status)) {
        ++report->metadata_errors;
        return INFS_STATUS_OK;
    }
    return status;
}

infs_status infs_scrub(struct infs_volume *vol,
                       struct infs_scrub_report *report)
{
    if (!vol || !report)
        return INFS_STATUS_INVALID_ARGUMENT;
    memset(report, 0, sizeof(*report));
    report->scrub_generation = infs_le64_to_cpu(vol->sb.generation);

    /* Whole-volume graph validation belongs here rather than in the normal
     * device-open path. Scrub is explicitly the expensive consistency pass:
     * verify index/object agreement, bitmap ownership, namespace reachability,
     * link counts, checksum ownership and checksum-chain structure before
     * reading every allocated data block below. */
    infs_status status = scrub_validate_full_metadata(vol, report);
    if (status != INFS_STATUS_OK || report->metadata_errors != 0)
        return status;

    struct infs_index_entry_disk *entries = NULL;
    uint32_t entries_count = 0;
    status = index_snapshot(vol, &entries, &entries_count);
    if (status != INFS_STATUS_OK)
        return status;

    for (uint32_t i = 0; i < entries_count; ++i) {
        if (infs_le16_to_cpu(entries[i].object_type) != INFS_OBJECT_FILE)
            continue;
        ++report->files_checked;

        uint64_t object_block = infs_le64_to_cpu(entries[i].object_block);
        uint8_t object[INFS_BLOCK_SIZE];
        if (read_object(vol, object_block, object) != INFS_STATUS_OK) {
            ++report->metadata_errors;
            continue;
        }
        struct infs_object_header_disk *hdr =
            (struct infs_object_header_disk *)object;
        struct infs_file_payload_disk *file;
        struct infs_extent_disk *ext;
        if (file_validate_volume(vol, object, &file, &ext) != INFS_STATUS_OK ||
            memcmp(hdr->object_id, entries[i].object_id, 16) != 0) {
            ++report->metadata_errors;
            continue;
        }

        if (file_is_inline(object, file)) {
            ++report->data_blocks_checked;
            continue;
        }

        struct infs_extent_disk *scan_extents = ext;
        uint32_t extent_count = infs_le32_to_cpu(file->extent_count);
        int owned_extents = 0;
        status = file_extent_snapshot(vol, object, file, ext,
                                      &scan_extents, &extent_count,
                                      &owned_extents);
        if (status != INFS_STATUS_OK) {
            ++report->metadata_errors;
            continue;
        }
        for (uint32_t j = 0; j < extent_count; ++j) {
            if (infs_le32_to_cpu(scan_extents[j].flags) != INFS_EXTENT_NORMAL)
                continue;
            uint64_t logical =
                infs_le64_to_cpu(scan_extents[j].logical_block);
            uint64_t physical =
                infs_le64_to_cpu(scan_extents[j].physical_block);
            uint32_t blocks =
                infs_le32_to_cpu(scan_extents[j].block_count);
            for (uint32_t k = 0; k < blocks; ++k) {
                ++report->data_blocks_checked;
                uint8_t data[INFS_BLOCK_SIZE];
                if (read_block(vol, physical + k, data) != INFS_STATUS_OK) {
                    ++report->metadata_errors;
                    continue;
                }
                struct infs_data_checksum_disk expected, actual;
                if (checksum_get(vol, file, hdr->object_id, logical + k,
                                 &expected) != INFS_STATUS_OK) {
                    ++report->metadata_errors;
                    continue;
                }
                data_block_checksum(data, &actual);
                if (memcmp(&expected, &actual, sizeof(expected)) != 0)
                    ++report->checksum_errors;
            }
        }
        if (owned_extents)
            free(scan_extents);
    }
    free(entries);

    if (snapshots_enabled(vol)) {
        int owns_context = vol->snapshot_scrub_context == NULL;
        struct snapshot_visit_context *visit = vol->snapshot_scrub_context;
        if (owns_context) {
            visit = calloc(1, sizeof(*visit));
            if (!visit)
                return INFS_STATUS_NO_MEMORY;
            vol->snapshot_scrub_context = visit;
        }
        uint8_t catalog[INFS_BLOCK_SIZE];
        struct infs_snapshot_catalog_payload_disk *payload;
        status = snapshot_catalog_load(vol, catalog, &payload, NULL);
        int have_catalog = status == INFS_STATUS_OK;
        if (status == INFS_STATUS_NOT_FOUND)
            status = INFS_STATUS_OK;
        if (status == INFS_STATUS_OK && have_catalog) {
            uint32_t count = infs_le32_to_cpu(payload->snapshot_count);
            struct infs_snapshot_record_disk *records =
                (struct infs_snapshot_record_disk *)(payload + 1);
            for (uint32_t i = 0; i < count; ++i) {
                int first_visit = 0;
                status = snapshot_visit_once(
                    visit, &records[i], &first_visit);
                if (status != INFS_STATUS_OK)
                    break;
                if (!first_visit)
                    continue;
                ++report->snapshots_checked;
                char name[INFS_SNAPSHOT_NAME_MAX + 1u] = {0};
                uint16_t length = infs_le16_to_cpu(records[i].name_length);
                memcpy(name, records[i].name, length);
                struct infs_volume view;
                status = snapshot_open_view(vol, name, &view);
                if (status != INFS_STATUS_OK)
                    break;
                struct infs_scrub_report historical;
                status = infs_scrub(&view, &historical);
                snapshot_close_view(&view);
                if (status != INFS_STATUS_OK)
                    break;
                report->files_checked += historical.files_checked;
                report->data_blocks_checked += historical.data_blocks_checked;
                report->checksum_errors += historical.checksum_errors;
                report->metadata_errors += historical.metadata_errors;
                report->snapshots_checked += historical.snapshots_checked;
            }
        }
        if (owns_context) {
            vol->snapshot_scrub_context = NULL;
            snapshot_visit_context_destroy(visit);
        }
        if (status != INFS_STATUS_OK)
            return status;
    }
    return INFS_STATUS_OK;
}


infs_status infs_snapshot_scrub(struct infs_volume *vol, const char *snapshot,
                                struct infs_scrub_report *report)
{
    if (!vol || !snapshot || !report)
        return INFS_STATUS_INVALID_ARGUMENT;
    struct infs_volume view;
    infs_status status = snapshot_open_view(vol, snapshot, &view);
    if (status != INFS_STATUS_OK)
        return status;
    status = infs_scrub(&view, report);
    snapshot_close_view(&view);
    return status;
}

static infs_status online_scrub_unique_name(struct infs_volume *vol,
                                            char name[INFS_SNAPSHOT_NAME_MAX + 1u])
{
    static const char hex[] = "0123456789abcdef";
    const char prefix[] = ".infilfs-online-scrub-";
    uint8_t nonce[12];
    for (unsigned attempt = 0; attempt < 8u; ++attempt) {
        infs_status status = infs_storage_random(&vol->storage, nonce,
                                                 sizeof(nonce));
        if (status != INFS_STATUS_OK)
            return status;
        size_t at = 0;
        memcpy(name, prefix, sizeof(prefix) - 1u);
        at += sizeof(prefix) - 1u;
        for (size_t i = 0; i < sizeof(nonce); ++i) {
            name[at++] = hex[nonce[i] >> 4];
            name[at++] = hex[nonce[i] & 0x0fu];
        }
        name[at] = '\0';
        struct infs_snapshot_record_disk record;
        status = snapshot_find_record(vol, name, &record);
        if (status == INFS_STATUS_NOT_FOUND)
            return INFS_STATUS_OK;
        if (status != INFS_STATUS_OK)
            return status;
    }
    return INFS_STATUS_ALREADY_EXISTS;
}

infs_status infs_scrub_online(struct infs_volume *vol,
                              struct infs_scrub_report *report)
{
    if (!vol || !report)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (!snapshots_enabled(vol))
        return INFS_STATUS_NOT_SUPPORTED;
    if (!vol->writable)
        return INFS_STATUS_READ_ONLY;

    char snapshot[INFS_SNAPSHOT_NAME_MAX + 1u];
    infs_status status = online_scrub_unique_name(vol, snapshot);
    if (status != INFS_STATUS_OK)
        return status;
    status = infs_snapshot_create(vol, snapshot);
    if (status != INFS_STATUS_OK)
        return status;

    infs_status scrub_status = infs_snapshot_scrub(vol, snapshot, report);
    infs_status cleanup_status = infs_snapshot_delete(vol, snapshot);
    if (scrub_status != INFS_STATUS_OK)
        return scrub_status;
    return cleanup_status;
}
