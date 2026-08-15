// SPDX-License-Identifier: GPL-3.0-or-later
static int extent_emit(struct infs_extent_disk *out, uint32_t *count,
                       uint64_t logical, uint64_t physical, uint32_t blocks,
                       uint32_t flags)
{
    if (!blocks)
        return 0;
    if (*count) {
        struct infs_extent_disk *last = &out[*count - 1u];
        if (infs_le32_to_cpu(last->flags) == flags &&
            infs_le64_to_cpu(last->logical_block) + infs_le32_to_cpu(last->block_count) == logical &&
            infs_le64_to_cpu(last->physical_block) + infs_le32_to_cpu(last->block_count) == physical) {
            uint64_t merged = (uint64_t)infs_le32_to_cpu(last->block_count) + blocks;
            if (merged <= UINT32_MAX) {
                last->block_count = infs_cpu_to_le32((uint32_t)merged);
                return 0;
            }
        }
    }
    if (*count >= INFS_MAX_FILE_EXTENTS) {
        return INFS_STATUS_FILE_TOO_LARGE;
    }
    out[*count].logical_block = infs_cpu_to_le64(logical);
    out[*count].physical_block = infs_cpu_to_le64(physical);
    out[*count].block_count = infs_cpu_to_le32(blocks);
    out[*count].flags = infs_cpu_to_le32(flags);
    ++*count;
    return 0;
}

static int file_replace_range(struct infs_volume *vol,
                              uint8_t object[INFS_BLOCK_SIZE],
                              uint64_t logical_start, uint64_t block_count,
                              uint64_t new_physical)
{
    struct infs_file_payload_disk *p =
        (struct infs_file_payload_disk *)(object + sizeof(struct infs_object_header_disk));
    struct infs_extent_disk *ext = (struct infs_extent_disk *)(p + 1);
    uint32_t old_count = infs_le32_to_cpu(p->extent_count);
    struct infs_extent_disk rebuilt[INFS_MAX_FILE_EXTENTS];
    uint32_t new_count = 0;
    uint64_t replace_end = logical_start + block_count;
    int replacement_emitted = 0;

    if (replace_end < logical_start) {
        return INFS_STATUS_INVALID_ARGUMENT;
    }

    for (uint32_t i = 0; i < old_count; ++i) {
        uint64_t l = infs_le64_to_cpu(ext[i].logical_block);
        uint64_t phys = infs_le64_to_cpu(ext[i].physical_block);
        uint32_t n = infs_le32_to_cpu(ext[i].block_count);
        uint32_t flags = infs_le32_to_cpu(ext[i].flags);
        uint64_t e = l + n;
        if (e <= logical_start || l >= replace_end) {
            if (extent_emit(rebuilt, &new_count, l, phys, n, flags) != 0)
                return INFS_STATUS_ERROR;
            continue;
        }
        if (l < logical_start) {
            uint32_t prefix = (uint32_t)(logical_start - l);
            if (extent_emit(rebuilt, &new_count, l, phys, prefix, flags) != 0)
                return INFS_STATUS_ERROR;
        }
        uint64_t overlap_start = l > logical_start ? l : logical_start;
        uint64_t overlap_end = e < replace_end ? e : replace_end;
        if (!replacement_emitted) {
            if (extent_emit(rebuilt, &new_count, logical_start, new_physical,
                            (uint32_t)block_count, INFS_EXTENT_NORMAL) != 0)
                return INFS_STATUS_ERROR;
            replacement_emitted = 1;
        }
        free_run(vol, phys + (overlap_start - l), overlap_end - overlap_start);
        if (e > replace_end) {
            uint32_t suffix = (uint32_t)(e - replace_end);
            if (extent_emit(rebuilt, &new_count, replace_end,
                            phys + (replace_end - l), suffix, flags) != 0)
                return INFS_STATUS_ERROR;
        }
    }
    if (!replacement_emitted) {
        return INFS_STATUS_CORRUPT;
    }

    memset(ext, 0, (size_t)INFS_MAX_FILE_EXTENTS * sizeof(*ext));
    memcpy(ext, rebuilt, (size_t)new_count * sizeof(*ext));
    p->extent_count = infs_cpu_to_le32(new_count);
    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)object;
    hdr->payload_size = infs_cpu_to_le32((uint32_t)(sizeof(*p) +
        (size_t)new_count * sizeof(*ext)));
    return 0;
}

static int data_block_verify(struct infs_volume *vol,
                             struct infs_file_payload_disk *file,
                             const uint8_t owner_id[16], uint64_t logical,
                             const uint8_t data[INFS_BLOCK_SIZE])
{
    struct infs_data_checksum_disk expected, actual;
    if (checksum_get(vol, file, owner_id, logical, &expected) != 0)
        return INFS_STATUS_ERROR;
    data_block_checksum(data, &actual);
    if (memcmp(&expected, &actual, sizeof(expected)) != 0) {
        return INFS_STATUS_CORRUPT;
    }
    return 0;
}
