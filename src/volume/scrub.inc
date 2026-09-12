// SPDX-License-Identifier: GPL-3.0-or-later
static int scrub_reportable_metadata_status(infs_status status)
{
    return status == INFS_STATUS_CORRUPT ||
           status == INFS_STATUS_NOT_FOUND ||
           status == INFS_STATUS_LOOP_DETECTED;
}

static void scrub_emit_progress(infs_scrub_progress_fn progress, void *context,
                                uint32_t phase,
                                const struct infs_scrub_report *report)
{
    if (!progress || !report)
        return;
    struct infs_scrub_progress state = {
        .phase = phase,
        .metadata_stage = INFS_SCRUB_METADATA_NONE,
        .metadata_generation = report->scrub_generation,
        .metadata_items_checked = 0,
        .metadata_items_total = 0,
        .files_checked = report->files_checked,
        .data_blocks_checked = report->data_blocks_checked,
        .snapshots_checked = report->snapshots_checked,
        .checksum_errors = report->checksum_errors,
        .metadata_errors = report->metadata_errors,
    };
    progress(&state, context);
}

static void scrub_metadata_progress_emit(
    struct scrub_metadata_progress_context *progress)
{
    if (!progress || !progress->callback || !progress->report)
        return;
    struct infs_scrub_progress state = {
        .phase = INFS_SCRUB_PHASE_METADATA,
        .metadata_stage = progress->stage,
        .metadata_generation = progress->generation,
        .metadata_items_checked = progress->done,
        .metadata_items_total = progress->total,
        .files_checked = progress->report->files_checked,
        .data_blocks_checked = progress->report->data_blocks_checked,
        .snapshots_checked = progress->report->snapshots_checked,
        .checksum_errors = progress->report->checksum_errors,
        .metadata_errors = progress->report->metadata_errors,
    };
    progress->callback(&state, progress->callback_context);
}

static void scrub_metadata_progress_begin_generation(
    struct scrub_metadata_progress_context *progress,
    uint64_t generation, uint32_t stage, uint64_t total)
{
    if (!progress)
        return;
    progress->stage = stage;
    progress->generation = generation;
    progress->done = 0;
    progress->total = total;
    /* Bound callbacks to roughly 200 per sub-stage while still making tiny
     * jobs visibly move one item at a time. */
    progress->stride = total > 200u ? (total + 199u) / 200u : 1u;
    progress->next_emit = progress->stride;
    scrub_metadata_progress_emit(progress);
}

static void scrub_metadata_progress_begin(
    struct scrub_metadata_progress_context *progress,
    const struct infs_volume *vol, uint32_t stage, uint64_t total)
{
    scrub_metadata_progress_begin_generation(
        progress, vol ? infs_le64_to_cpu(vol->sb.generation) : 0,
        stage, total);
}

static void scrub_metadata_progress_update(
    struct scrub_metadata_progress_context *progress, uint64_t done)
{
    if (!progress)
        return;
    if (progress->total && done > progress->total)
        done = progress->total;
    progress->done = done;
    if (done < progress->next_emit &&
        (!progress->total || done != progress->total))
        return;
    scrub_metadata_progress_emit(progress);
    if (UINT64_MAX - done < progress->stride)
        progress->next_emit = UINT64_MAX;
    else
        progress->next_emit = done + progress->stride;
}

static infs_status scrub_validate_generation_metadata(
    struct infs_volume *vol, struct infs_scrub_report *report,
    struct scrub_metadata_progress_context *progress)
{
    infs_status status = validate_index_entries_progress(vol, progress);
    if (status == INFS_STATUS_OK)
        status = validate_integrity_metadata_progress(vol, progress, 0);
    if (status == INFS_STATUS_OK)
        status = validate_namespace_graph_progress(vol, progress);
    if (status == INFS_STATUS_OK)
        status = validate_checksum_graph_progress(vol, progress);

    if (status == INFS_STATUS_OK)
        return INFS_STATUS_OK;
    if (scrub_reportable_metadata_status(status)) {
        ++report->metadata_errors;
        if (progress)
            scrub_metadata_progress_emit(progress);
        return INFS_STATUS_OK;
    }
    return status;
}

/* Walk retained generations explicitly during scrub instead of allowing the
 * generic integrity validator to recurse invisibly. This gives every retained
 * generation the same detailed metadata progress instrumentation and keeps the
 * visited-generation guard that prevents duplicate DAG traversal. */
static infs_status scrub_validate_snapshot_metadata(
    struct infs_volume *vol, struct infs_scrub_report *report,
    struct snapshot_visit_context *visit,
    struct scrub_metadata_progress_context *progress)
{
    if (!snapshots_enabled(vol))
        return INFS_STATUS_OK;

    uint8_t catalog[INFS_BLOCK_SIZE];
    struct infs_snapshot_catalog_payload_disk *payload;
    infs_status status = snapshot_catalog_load(vol, catalog, &payload, NULL);
    if (status == INFS_STATUS_NOT_FOUND)
        return INFS_STATUS_OK;
    if (status != INFS_STATUS_OK)
        return status;

    uint32_t count = infs_le32_to_cpu(payload->snapshot_count);
    struct infs_snapshot_record_disk *records =
        (struct infs_snapshot_record_disk *)(payload + 1);

    for (uint32_t i = 0; i < count; ++i) {
        int first_visit = 0;
        status = snapshot_visit_once(visit, &records[i], &first_visit);
        if (status != INFS_STATUS_OK)
            return status;
        if (!first_visit)
            continue;

        uint64_t generation = infs_le64_to_cpu(records[i].generation);
        scrub_metadata_progress_begin_generation(
            progress, generation, INFS_SCRUB_METADATA_SNAPSHOT_GENERATIONS,
            count);
        scrub_metadata_progress_update(progress, (uint64_t)i + 1u);

        char name[INFS_SNAPSHOT_NAME_MAX + 1u] = {0};
        uint16_t length = infs_le16_to_cpu(records[i].name_length);
        memcpy(name, records[i].name, length);
        struct infs_volume view;
        status = snapshot_open_view(vol, name, &view);
        if (status != INFS_STATUS_OK)
            return status;

        status = scrub_validate_generation_metadata(&view, report, progress);
        if (status == INFS_STATUS_OK && report->metadata_errors == 0)
            status = scrub_validate_snapshot_metadata(
                &view, report, visit, progress);
        snapshot_close_view(&view);
        if (status != INFS_STATUS_OK || report->metadata_errors != 0)
            return status;
    }
    return INFS_STATUS_OK;
}

/* Verify file payloads for one already metadata-validated generation. Report
 * counters are additive so the caller can accumulate live and snapshot work
 * without recursively repeating graph validators. */
static infs_status scrub_verify_data(
    struct infs_volume *vol, struct infs_scrub_report *report,
    infs_scrub_progress_fn progress, void *context, uint32_t phase)
{
    struct infs_index_entry_disk *entries = NULL;
    uint32_t entries_count = 0;
    infs_status status = index_snapshot(vol, &entries, &entries_count);
    if (status != INFS_STATUS_OK)
        return status;

    uint64_t next_block_report = report->data_blocks_checked + UINT64_C(4096);
    uint64_t next_file_report = report->files_checked + UINT64_C(128);

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
            if (report->files_checked >= next_file_report ||
                report->data_blocks_checked >= next_block_report) {
                scrub_emit_progress(progress, context, phase, report);
                next_file_report = report->files_checked + UINT64_C(128);
                next_block_report =
                    report->data_blocks_checked + UINT64_C(4096);
            }
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
            uint32_t flags = infs_le32_to_cpu(scan_extents[j].flags);
            if (extent_kind(flags) != INFS_EXTENT_NORMAL)
                continue;
            uint64_t logical =
                infs_le64_to_cpu(scan_extents[j].logical_block);
            uint64_t physical =
                infs_le64_to_cpu(scan_extents[j].physical_block);
            uint32_t blocks =
                infs_le32_to_cpu(scan_extents[j].block_count);
            uint8_t *plain = NULL;
            if (extent_is_compressed(flags)) {
                size_t plain_bytes = 0;
                if (file_decompress_extent(
                        vol, &scan_extents[j], &plain, &plain_bytes) !=
                        INFS_STATUS_OK ||
                    plain_bytes != (size_t)blocks * INFS_BLOCK_SIZE) {
                    free(plain);
                    ++report->checksum_errors;
                    continue;
                }
            }

            for (uint32_t k = 0; k < blocks;) {
                uint64_t current_logical = logical + k;
                uint64_t checksum_offset =
                    current_logical % INFS_CHECKSUMS_PER_OBJECT;
                uint64_t available =
                    INFS_CHECKSUMS_PER_OBJECT - checksum_offset;
                uint32_t chunk = blocks - k;
                if ((uint64_t)chunk > available)
                    chunk = (uint32_t)available;

                uint8_t checksum_block[INFS_BLOCK_SIZE];
                struct infs_checksum_payload_disk *checksum;
                struct infs_data_checksum_disk *expected;
                infs_status checksum_status = checksum_locate(
                    vol, file, hdr->object_id, current_logical, 0, NULL,
                    checksum_block, &checksum, &expected);
                uint32_t checksum_count = checksum_status == INFS_STATUS_OK ?
                    infs_le32_to_cpu(checksum->checksum_count) : 0;
                if (checksum_status != INFS_STATUS_OK ||
                    checksum_offset + chunk > checksum_count) {
                    report->metadata_errors += chunk;
                    report->data_blocks_checked += chunk;
                    k += chunk;
                    if (report->data_blocks_checked >= next_block_report) {
                        scrub_emit_progress(progress, context, phase, report);
                        next_block_report =
                            report->data_blocks_checked + UINT64_C(4096);
                    }
                    continue;
                }

                for (uint32_t n = 0; n < chunk; ++n) {
                    ++report->data_blocks_checked;
                    uint8_t data[INFS_BLOCK_SIZE];
                    uint32_t block_index = k + n;
                    if (plain) {
                        memcpy(data,
                               plain + (size_t)block_index * INFS_BLOCK_SIZE,
                               INFS_BLOCK_SIZE);
                    } else if (read_block(vol, physical + block_index, data) !=
                               INFS_STATUS_OK) {
                        ++report->metadata_errors;
                        continue;
                    }
                    struct infs_data_checksum_disk actual;
                    data_block_checksum(data, &actual);
                    if (memcmp(&expected[checksum_offset + n], &actual,
                               sizeof(actual)) != 0)
                        ++report->checksum_errors;
                }
                k += chunk;
                if (report->data_blocks_checked >= next_block_report) {
                    scrub_emit_progress(progress, context, phase, report);
                    next_block_report =
                        report->data_blocks_checked + UINT64_C(4096);
                }
            }
            free(plain);
        }
        if (owned_extents)
            free(scan_extents);

        if (report->files_checked >= next_file_report) {
            scrub_emit_progress(progress, context, phase, report);
            next_file_report = report->files_checked + UINT64_C(128);
        }
    }
    free(entries);
    scrub_emit_progress(progress, context, phase, report);
    return INFS_STATUS_OK;
}

static infs_status scrub_verify_snapshot_data(
    struct infs_volume *vol, struct infs_scrub_report *report,
    struct snapshot_visit_context *visit, infs_scrub_progress_fn progress,
    void *context)
{
    if (!snapshots_enabled(vol))
        return INFS_STATUS_OK;

    uint8_t catalog[INFS_BLOCK_SIZE];
    struct infs_snapshot_catalog_payload_disk *payload;
    infs_status status = snapshot_catalog_load(vol, catalog, &payload, NULL);
    if (status == INFS_STATUS_NOT_FOUND)
        return INFS_STATUS_OK;
    if (status != INFS_STATUS_OK)
        return status;

    uint32_t count = infs_le32_to_cpu(payload->snapshot_count);
    struct infs_snapshot_record_disk *records =
        (struct infs_snapshot_record_disk *)(payload + 1);
    for (uint32_t i = 0; i < count; ++i) {
        int first_visit = 0;
        status = snapshot_visit_once(visit, &records[i], &first_visit);
        if (status != INFS_STATUS_OK)
            return status;
        if (!first_visit)
            continue;

        ++report->snapshots_checked;
        scrub_emit_progress(progress, context, INFS_SCRUB_PHASE_SNAPSHOTS,
                            report);

        char name[INFS_SNAPSHOT_NAME_MAX + 1u] = {0};
        uint16_t length = infs_le16_to_cpu(records[i].name_length);
        memcpy(name, records[i].name, length);
        struct infs_volume view;
        status = snapshot_open_view(vol, name, &view);
        if (status != INFS_STATUS_OK)
            return status;

        status = scrub_verify_data(&view, report, progress, context,
                                   INFS_SCRUB_PHASE_SNAPSHOTS);
        if (status == INFS_STATUS_OK)
            status = scrub_verify_snapshot_data(
                &view, report, visit, progress, context);
        snapshot_close_view(&view);
        if (status != INFS_STATUS_OK)
            return status;
    }
    return INFS_STATUS_OK;
}

static infs_status scrub_run(struct infs_volume *vol,
                             struct infs_scrub_report *report,
                             infs_scrub_progress_fn progress, void *context)
{
    if (!vol || !report)
        return INFS_STATUS_INVALID_ARGUMENT;
    memset(report, 0, sizeof(*report));
    report->scrub_generation = infs_le64_to_cpu(vol->sb.generation);

    struct scrub_metadata_progress_context metadata_progress = {
        .callback = progress,
        .callback_context = context,
        .report = report,
        .generation = report->scrub_generation,
    };

    /* Validate the live generation and every reachable retained generation
     * with sub-stage progress. The generic integrity validator's historical
     * recursion is deliberately disabled here so no expensive work is hidden
     * behind a static progress line. */
    infs_status status = scrub_validate_generation_metadata(
        vol, report, &metadata_progress);
    if (status != INFS_STATUS_OK || report->metadata_errors != 0)
        return status;

    if (snapshots_enabled(vol)) {
        struct snapshot_visit_context *visit = calloc(1, sizeof(*visit));
        if (!visit)
            return INFS_STATUS_NO_MEMORY;
        status = scrub_validate_snapshot_metadata(
            vol, report, visit, &metadata_progress);
        snapshot_visit_context_destroy(visit);
        if (status != INFS_STATUS_OK || report->metadata_errors != 0)
            return status;
    }

    scrub_emit_progress(progress, context, INFS_SCRUB_PHASE_DATA, report);
    status = scrub_verify_data(vol, report, progress, context,
                               INFS_SCRUB_PHASE_DATA);
    if (status != INFS_STATUS_OK)
        return status;

    if (snapshots_enabled(vol)) {
        int owns_context = vol->snapshot_scrub_context == NULL;
        struct snapshot_visit_context *visit = vol->snapshot_scrub_context;
        if (owns_context) {
            visit = calloc(1, sizeof(*visit));
            if (!visit)
                return INFS_STATUS_NO_MEMORY;
            vol->snapshot_scrub_context = visit;
        }
        status = scrub_verify_snapshot_data(
            vol, report, visit, progress, context);
        if (owns_context) {
            vol->snapshot_scrub_context = NULL;
            snapshot_visit_context_destroy(visit);
        }
        if (status != INFS_STATUS_OK)
            return status;
    }

    scrub_emit_progress(progress, context, INFS_SCRUB_PHASE_COMPLETE, report);
    return INFS_STATUS_OK;
}

infs_status infs_scrub(struct infs_volume *vol,
                       struct infs_scrub_report *report)
{
    return scrub_run(vol, report, NULL, NULL);
}

infs_status infs_scrub_with_progress(
    struct infs_volume *vol, struct infs_scrub_report *report,
    infs_scrub_progress_fn progress, void *context)
{
    return scrub_run(vol, report, progress, context);
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
