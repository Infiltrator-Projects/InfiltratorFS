// SPDX-License-Identifier: GPL-3.0-or-later
static int extent_emit(struct infs_extent_disk *out, uint32_t *count,
                       uint64_t logical, uint64_t physical, uint32_t blocks,
                       uint32_t flags)
{
    return extent_emit_capacity(out, count, INFS_MAX_FILE_EXTENTS,
                                logical, physical, blocks, flags);
}

static int file_replace_range(struct infs_volume *vol,
                              uint8_t object[INFS_BLOCK_SIZE],
                              uint64_t logical_start, uint64_t block_count,
                              uint64_t new_physical, uint32_t new_flags)
{
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    struct infs_file_payload_disk *p =
        (struct infs_file_payload_disk *)(object + sizeof(*hdr));
    uint64_t replace_end = logical_start + block_count;
    if (replace_end < logical_start || block_count > UINT32_MAX ||
        (new_flags != INFS_EXTENT_NORMAL && new_flags != INFS_EXTENT_HOLE) ||
        (new_flags == INFS_EXTENT_HOLE && new_physical != 0) ||
        (new_flags == INFS_EXTENT_NORMAL && new_physical == 0))
        return INFS_STATUS_INVALID_ARGUMENT;

    /* Random overwrites can repeatedly split non-tail extent pages inside one
     * deferred transaction.  Rebuilding from the complete ordered snapshot is
     * deliberately used here until the single-page fast path can prove the
     * same transaction/savepoint invariants.  The full rebuild is copy-on-
     * write and keeps every prior published page untouched until the new file
     * head is safely written, so a failed operation cannot poison the last
     * committed generation. */
    if (file_has_paged_extents(object))
        return paged_extent_replace_rebuild(
            vol, object, p, logical_start, block_count,
            new_physical, new_flags);

    struct infs_extent_disk *ext = (struct infs_extent_disk *)(p + 1);
    uint32_t old_count = infs_le32_to_cpu(p->extent_count);
    uint32_t capacity = old_count + 3u;
    struct infs_extent_disk *rebuilt = calloc(capacity, sizeof(*rebuilt));
    if (!rebuilt)
        return INFS_STATUS_NO_MEMORY;
    struct { uint64_t start, count; } *release = calloc(
        old_count ? old_count : 1u, sizeof(*release));
    if (!release) {
        free(rebuilt);
        return INFS_STATUS_NO_MEMORY;
    }
    uint32_t new_count = 0, release_count = 0;
    int emitted = 0;
    infs_status status = INFS_STATUS_OK;
    for (uint32_t i = 0; i < old_count; ++i) {
        uint64_t l = infs_le64_to_cpu(ext[i].logical_block);
        uint64_t phys = infs_le64_to_cpu(ext[i].physical_block);
        uint32_t n = infs_le32_to_cpu(ext[i].block_count);
        uint32_t flags = infs_le32_to_cpu(ext[i].flags);
        uint64_t e = l + n;
        if (e <= logical_start || l >= replace_end) {
            status = extent_emit_capacity(rebuilt, &new_count, capacity,
                                          l, phys, n, flags);
            if (status != INFS_STATUS_OK)
                goto out;
            continue;
        }
        if (l < logical_start) {
            status = extent_emit_capacity(rebuilt, &new_count, capacity,
                l, phys, (uint32_t)(logical_start - l), flags);
            if (status != INFS_STATUS_OK)
                goto out;
        }
        uint64_t overlap_start = l > logical_start ? l : logical_start;
        uint64_t overlap_end = e < replace_end ? e : replace_end;
        if (!emitted) {
            status = extent_emit_capacity(rebuilt, &new_count, capacity,
                logical_start, new_physical, (uint32_t)block_count, new_flags);
            if (status != INFS_STATUS_OK)
                goto out;
            emitted = 1;
        }
        if (flags == INFS_EXTENT_NORMAL) {
            release[release_count].start = phys + (overlap_start - l);
            release[release_count].count = overlap_end - overlap_start;
            ++release_count;
        }
        if (e > replace_end) {
            status = extent_emit_capacity(rebuilt, &new_count, capacity,
                replace_end, flags == INFS_EXTENT_HOLE ? 0 :
                    phys + (replace_end - l),
                (uint32_t)(e - replace_end), flags);
            if (status != INFS_STATUS_OK)
                goto out;
        }
    }
    if (!emitted) {
        status = INFS_STATUS_CORRUPT;
        goto out;
    }
    if (new_count <= INFS_MAX_FILE_EXTENTS) {
        memset(ext, 0, (size_t)INFS_MAX_FILE_EXTENTS * sizeof(*ext));
        memcpy(ext, rebuilt, (size_t)new_count * sizeof(*ext));
        p->extent_count = infs_cpu_to_le32(new_count);
        hdr->payload_size = infs_cpu_to_le32((uint32_t)(
            sizeof(*p) + (size_t)new_count * sizeof(*ext)));
    } else {
        status = paged_extent_rewrite_all(
            vol, object, p, rebuilt, new_count, 1);
        if (status != INFS_STATUS_OK)
            goto out;
    }
    for (uint32_t i = 0; i < release_count; ++i) {
        status = file_free_unshared_run(vol, hdr->object_id,
                                        release[i].start, release[i].count);
        if (status != INFS_STATUS_OK)
            goto out;
    }
out:
    free(release);
    free(rebuilt);
    return status;
}

static int data_block_verify(struct infs_volume *vol,
                             struct infs_file_payload_disk *file,
                             const uint8_t owner_id[16], uint64_t logical,
                             const uint8_t data[INFS_BLOCK_SIZE])
{
    struct infs_data_checksum_disk expected, actual;
    infs_status status = checksum_get(vol, file, owner_id, logical, &expected);
    if (status != INFS_STATUS_OK)
        return status;
    data_block_checksum(data, &actual);
    if (memcmp(&expected, &actual, sizeof(expected)) != 0)
        return INFS_STATUS_CORRUPT;
    return INFS_STATUS_OK;
}
