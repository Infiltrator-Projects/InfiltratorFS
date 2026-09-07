// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Rebuildable live shared-extent reference index.
 *
 * The allocation bitmap stays authoritative. This interval map turns reflink
 * release from repeated whole-filesystem scans into one graph rebuild followed
 * by logarithmic interval lookups for the mutation.
 */
struct infs_shared_ref_run {
    uint64_t start;
    uint64_t end;
    uint32_t refs;
};

struct infs_shared_ref_event {
    uint64_t block;
    int64_t delta;
};

static void shared_ref_index_invalidate(struct infs_volume *vol)
{
    if (vol)
        vol->shared_ref_index_valid = 0;
}

static void shared_ref_index_destroy(struct infs_volume *vol)
{
    if (!vol)
        return;
    free(vol->shared_refs);
    vol->shared_refs = NULL;
    vol->shared_ref_count = 0;
    vol->shared_ref_capacity = 0;
    vol->shared_ref_index_valid = 0;
}

static int shared_ref_event_compare(const void *a, const void *b)
{
    const struct infs_shared_ref_event *ea = a;
    const struct infs_shared_ref_event *eb = b;
    return ea->block < eb->block ? -1 : ea->block > eb->block ? 1 : 0;
}

static infs_status shared_ref_reserve(struct infs_volume *vol, size_t needed)
{
    struct infs_shared_ref_run *grown;
    size_t next;

    if (needed <= vol->shared_ref_capacity)
        return INFS_STATUS_OK;
    next = vol->shared_ref_capacity ? vol->shared_ref_capacity : 64u;
    while (next < needed) {
        if (next > SIZE_MAX / 2u)
            return INFS_STATUS_OVERFLOW;
        next *= 2u;
    }
    if (next > SIZE_MAX / sizeof(*grown))
        return INFS_STATUS_OVERFLOW;
    grown = realloc(vol->shared_refs, next * sizeof(*grown));
    if (!grown)
        return INFS_STATUS_NO_MEMORY;
    vol->shared_refs = grown;
    vol->shared_ref_capacity = next;
    return INFS_STATUS_OK;
}

static infs_status shared_ref_event_push(
    struct infs_shared_ref_event **events, size_t *count, size_t *capacity,
    uint64_t block, int64_t delta)
{
    if (*count == *capacity) {
        struct infs_shared_ref_event *grown;
        size_t next = *capacity ? *capacity * 2u : 128u;
        if (next < *capacity || next > SIZE_MAX / sizeof(*grown))
            return INFS_STATUS_OVERFLOW;
        grown = realloc(*events, next * sizeof(*grown));
        if (!grown)
            return INFS_STATUS_NO_MEMORY;
        *events = grown;
        *capacity = next;
    }
    (*events)[*count].block = block;
    (*events)[*count].delta = delta;
    ++*count;
    return INFS_STATUS_OK;
}

static infs_status shared_ref_index_rebuild(struct infs_volume *vol)
{
    struct infs_index_entry_disk *entries = NULL;
    struct infs_shared_ref_event *events = NULL;
    uint32_t entry_count = 0;
    size_t event_count = 0, event_capacity = 0;
    infs_status status = index_snapshot(vol, &entries, &entry_count);

    if (status != INFS_STATUS_OK)
        return status;
    vol->shared_ref_count = 0;

    for (uint32_t i = 0; i < entry_count; ++i) {
        uint8_t object[INFS_BLOCK_SIZE];
        struct infs_file_payload_disk *file;
        struct infs_extent_disk *ext;
        struct infs_extent_disk *all = NULL;
        uint32_t count = 0;
        int owned = 0;

        if (infs_le16_to_cpu(entries[i].object_type) != INFS_OBJECT_FILE)
            continue;
        status = read_object(vol, infs_le64_to_cpu(entries[i].object_block), object);
        if (status != INFS_STATUS_OK)
            goto out;
        status = file_validate_volume(vol, object, &file, &ext);
        if (status != INFS_STATUS_OK)
            goto out;
        status = file_extent_snapshot(vol, object, file, ext, &all, &count, &owned);
        if (status != INFS_STATUS_OK)
            goto out;
        for (uint32_t j = 0; j < count; ++j) {
            uint32_t flags = infs_le32_to_cpu(all[j].flags);
            uint32_t logical_blocks = infs_le32_to_cpu(all[j].block_count);
            uint64_t start, blocks;

            if (extent_kind(flags) != INFS_EXTENT_NORMAL)
                continue;
            start = infs_le64_to_cpu(all[j].physical_block);
            blocks = extent_physical_blocks(logical_blocks, flags);
            if (!blocks || start > UINT64_MAX - blocks) {
                status = INFS_STATUS_CORRUPT;
                break;
            }
            status = shared_ref_event_push(&events, &event_count, &event_capacity,
                                           start, 1);
            if (status == INFS_STATUS_OK)
                status = shared_ref_event_push(&events, &event_count, &event_capacity,
                                               start + blocks, -1);
            if (status != INFS_STATUS_OK)
                break;
        }
        if (owned)
            free(all);
        if (status != INFS_STATUS_OK)
            goto out;
    }

    qsort(events, event_count, sizeof(*events), shared_ref_event_compare);
    {
        uint64_t cursor = 0;
        int64_t refs = 0;
        size_t i = 0;
        while (i < event_count) {
            uint64_t at = events[i].block;
            int64_t delta = 0;

            if (refs > 0 && cursor < at) {
                struct infs_shared_ref_run *run;
                if ((uint64_t)refs > UINT32_MAX) {
                    status = INFS_STATUS_OVERFLOW;
                    goto out;
                }
                status = shared_ref_reserve(vol, vol->shared_ref_count + 1u);
                if (status != INFS_STATUS_OK)
                    goto out;
                run = &vol->shared_refs[vol->shared_ref_count++];
                run->start = cursor;
                run->end = at;
                run->refs = (uint32_t)refs;
            }
            while (i < event_count && events[i].block == at) {
                delta += events[i].delta;
                ++i;
            }
            if ((delta < 0 && refs < -delta) ||
                (delta > 0 && refs > INT64_MAX - delta)) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            refs += delta;
            if (refs < 0) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            cursor = at;
        }
        if (refs != 0) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }
    }
    vol->shared_ref_index_valid = 1;
    status = INFS_STATUS_OK;
out:
    if (status != INFS_STATUS_OK)
        vol->shared_ref_index_valid = 0;
    free(events);
    free(entries);
    return status;
}

static infs_status shared_ref_index_lookup(
    struct infs_volume *vol, uint64_t block, uint64_t limit,
    uint64_t *interval_end, uint32_t *refs_out)
{
    size_t lo = 0, hi;
    infs_status status;

    if (!vol->shared_ref_index_valid) {
        status = shared_ref_index_rebuild(vol);
        if (status != INFS_STATUS_OK)
            return status;
    }
    hi = vol->shared_ref_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (vol->shared_refs[mid].end <= block)
            lo = mid + 1u;
        else
            hi = mid;
    }
    if (lo < vol->shared_ref_count && vol->shared_refs[lo].start <= block &&
        block < vol->shared_refs[lo].end) {
        *interval_end = vol->shared_refs[lo].end < limit ?
            vol->shared_refs[lo].end : limit;
        *refs_out = vol->shared_refs[lo].refs;
        return INFS_STATUS_OK;
    }
    *interval_end = limit;
    if (lo < vol->shared_ref_count && vol->shared_refs[lo].start < limit)
        *interval_end = vol->shared_refs[lo].start;
    *refs_out = 0;
    return INFS_STATUS_OK;
}
