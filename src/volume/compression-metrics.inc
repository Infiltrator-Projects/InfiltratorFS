// SPDX-License-Identifier: GPL-3.0-or-later
struct infs_metric_stream {
    uint64_t physical;
    uint64_t physical_blocks;
    uint64_t logical_blocks;
    uint32_t codec;
};

struct infs_metric_scan {
    struct infs_metric_stream *streams;
    size_t count;
    size_t capacity;
    struct infs_compression_metrics *metrics;
};

static int infs_metric_stream_compare(const void *left, const void *right)
{
    const struct infs_metric_stream *a = left;
    const struct infs_metric_stream *b = right;
    if (a->physical != b->physical)
        return a->physical < b->physical ? -1 : 1;
    if (a->physical_blocks != b->physical_blocks)
        return a->physical_blocks < b->physical_blocks ? -1 : 1;
    if (a->logical_blocks != b->logical_blocks)
        return a->logical_blocks < b->logical_blocks ? -1 : 1;
    if (a->codec != b->codec)
        return a->codec < b->codec ? -1 : 1;
    return 0;
}

static infs_status infs_metric_append_stream(
    struct infs_metric_scan *scan, uint64_t physical,
    uint64_t physical_blocks, uint64_t logical_blocks, uint32_t codec)
{
    if (scan->count == scan->capacity) {
        size_t next = scan->capacity ? scan->capacity * 2u : 256u;
        if (next < scan->capacity ||
            next > SIZE_MAX / sizeof(*scan->streams))
            return INFS_STATUS_OVERFLOW;
        void *grown = realloc(scan->streams, next * sizeof(*scan->streams));
        if (!grown)
            return INFS_STATUS_NO_MEMORY;
        scan->streams = grown;
        scan->capacity = next;
    }
    scan->streams[scan->count++] = (struct infs_metric_stream) {
        .physical = physical,
        .physical_blocks = physical_blocks,
        .logical_blocks = logical_blocks,
        .codec = codec,
    };
    return INFS_STATUS_OK;
}

static infs_status infs_metric_add_checked(uint64_t *value, uint64_t add)
{
    if (*value > UINT64_MAX - add)
        return INFS_STATUS_OVERFLOW;
    *value += add;
    return INFS_STATUS_OK;
}

static infs_status infs_metric_scan_view(
    struct infs_volume *vol, struct infs_metric_scan *scan)
{
    struct infs_index_entry_disk *entries = NULL;
    uint32_t entry_count = 0;
    infs_status status = index_snapshot(vol, &entries, &entry_count);
    if (status != INFS_STATUS_OK)
        return status;

    for (uint32_t i = 0; i < entry_count; ++i) {
        if (infs_le16_to_cpu(entries[i].object_type) != INFS_OBJECT_FILE)
            continue;

        uint8_t object[INFS_BLOCK_SIZE];
        struct infs_object_header_disk *header;
        struct infs_file_payload_disk *file;
        struct infs_extent_disk *extents;
        struct infs_extent_disk *owned_extents;
        uint32_t extent_count;
        int owns_extents = 0;

        status = read_object(
            vol, infs_le64_to_cpu(entries[i].object_block), object);
        if (status != INFS_STATUS_OK)
            goto out;
        header = (struct infs_object_header_disk *)object;
        if (memcmp(header->object_id, entries[i].object_id, 16) != 0) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }
        if (file_validate_volume(vol, object, &file, &extents) !=
            INFS_STATUS_OK) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }
        status = infs_metric_add_checked(
            &scan->metrics->referenced_logical_bytes,
            infs_le64_to_cpu(file->attributes.logical_size));
        if (status != INFS_STATUS_OK)
            goto out;
        status = infs_metric_add_checked(&scan->metrics->files_scanned, 1);
        if (status != INFS_STATUS_OK)
            goto out;
        if (file_is_inline(object, file))
            continue;

        extent_count = infs_le32_to_cpu(file->extent_count);
        owned_extents = extents;
        status = file_extent_snapshot(
            vol, object, file, extents, &owned_extents,
            &extent_count, &owns_extents);
        if (status != INFS_STATUS_OK)
            goto out;

        for (uint32_t j = 0; j < extent_count; ++j) {
            uint32_t flags = infs_le32_to_cpu(owned_extents[j].flags);
            uint64_t logical_blocks;
            uint64_t physical_blocks;
            uint64_t logical_bytes;

            if (!extent_is_compressed(flags))
                continue;
            logical_blocks =
                infs_le32_to_cpu(owned_extents[j].block_count);
            physical_blocks =
                extent_physical_blocks((uint32_t)logical_blocks, flags);
            if (!physical_blocks ||
                logical_blocks > UINT64_MAX / INFS_BLOCK_SIZE) {
                status = INFS_STATUS_CORRUPT;
                break;
            }
            logical_bytes = logical_blocks * INFS_BLOCK_SIZE;
            status = infs_metric_add_checked(
                &scan->metrics->compressed_referenced_logical_bytes,
                logical_bytes);
            if (status != INFS_STATUS_OK)
                break;
            status = infs_metric_append_stream(
                scan,
                infs_le64_to_cpu(owned_extents[j].physical_block),
                physical_blocks, logical_blocks, extent_codec(flags));
            if (status != INFS_STATUS_OK)
                break;
        }
        if (owns_extents)
            free(owned_extents);
        if (status != INFS_STATUS_OK)
            goto out;
    }
out:
    free(entries);
    return status;
}

infs_status infs_compression_metrics(
    struct infs_volume *vol, struct infs_compression_metrics *metrics)
{
    struct infs_metric_scan scan = {0};
    struct infs_snapshot_info *snapshots = NULL;
    size_t snapshot_count = 0;
    infs_status status;

    if (!vol || !metrics)
        return INFS_STATUS_INVALID_ARGUMENT;
    memset(metrics, 0, sizeof(*metrics));
    metrics->generation = infs_le64_to_cpu(vol->sb.generation);
    scan.metrics = metrics;

    status = infs_metric_scan_view(vol, &scan);
    if (status != INFS_STATUS_OK)
        goto out;

    status = infs_snapshot_list(vol, &snapshots, &snapshot_count);
    if (status != INFS_STATUS_OK)
        goto out;
    for (size_t i = 0; i < snapshot_count; ++i) {
        struct infs_volume view;
        status = snapshot_open_view(vol, snapshots[i].name, &view);
        if (status != INFS_STATUS_OK)
            goto out;
        status = infs_metric_scan_view(&view, &scan);
        snapshot_close_view(&view);
        if (status != INFS_STATUS_OK)
            goto out;
        status = infs_metric_add_checked(&metrics->snapshots_scanned, 1);
        if (status != INFS_STATUS_OK)
            goto out;
    }

    if (scan.count) {
        qsort(scan.streams, scan.count, sizeof(*scan.streams),
              infs_metric_stream_compare);
        for (size_t i = 0; i < scan.count; ++i) {
            if (i &&
                scan.streams[i].physical == scan.streams[i - 1].physical &&
                scan.streams[i].physical_blocks ==
                    scan.streams[i - 1].physical_blocks) {
                if (scan.streams[i].logical_blocks !=
                        scan.streams[i - 1].logical_blocks ||
                    scan.streams[i].codec != scan.streams[i - 1].codec) {
                    status = INFS_STATUS_CORRUPT;
                    goto out;
                }
                continue;
            }
            if (scan.streams[i].logical_blocks >
                    UINT64_MAX / INFS_BLOCK_SIZE ||
                scan.streams[i].physical_blocks >
                    UINT64_MAX / INFS_BLOCK_SIZE) {
                status = INFS_STATUS_OVERFLOW;
                goto out;
            }
            uint64_t logical =
                scan.streams[i].logical_blocks * INFS_BLOCK_SIZE;
            uint64_t physical =
                scan.streams[i].physical_blocks * INFS_BLOCK_SIZE;
            if (physical >= logical) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            status = infs_metric_add_checked(
                &metrics->unique_compressed_logical_bytes, logical);
            if (status == INFS_STATUS_OK)
                status = infs_metric_add_checked(
                    &metrics->unique_compressed_physical_bytes, physical);
            if (status == INFS_STATUS_OK)
                status = infs_metric_add_checked(
                    &metrics->compression_saved_bytes, logical - physical);
            if (status == INFS_STATUS_OK)
                status = infs_metric_add_checked(
                    &metrics->unique_compressed_streams, 1);
            if (status != INFS_STATUS_OK)
                goto out;
        }
    }
out:
    infs_free_snapshot_infos(snapshots);
    free(scan.streams);
    return status;
}
