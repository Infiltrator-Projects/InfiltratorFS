// SPDX-License-Identifier: GPL-3.0-or-later
static int extent_emit(struct infs_extent_disk *out, uint32_t *count,
                       uint64_t logical, uint64_t physical, uint32_t blocks,
                       uint32_t flags)
{
    if (!blocks)
        return 0;
    if (*count) {
        struct infs_extent_disk *last = &out[*count - 1u];
        if (le32toh(last->flags) == flags &&
            le64toh(last->logical_block) + le32toh(last->block_count) == logical &&
            le64toh(last->physical_block) + le32toh(last->block_count) == physical) {
            uint64_t merged = (uint64_t)le32toh(last->block_count) + blocks;
            if (merged <= UINT32_MAX) {
                last->block_count = htole32((uint32_t)merged);
                return 0;
            }
        }
    }
    if (*count >= INFS_MAX_FILE_EXTENTS) {
        errno = EFBIG;
        return -1;
    }
    out[*count].logical_block = htole64(logical);
    out[*count].physical_block = htole64(physical);
    out[*count].block_count = htole32(blocks);
    out[*count].flags = htole32(flags);
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
    uint32_t old_count = le32toh(p->extent_count);
    struct infs_extent_disk rebuilt[INFS_MAX_FILE_EXTENTS];
    uint32_t new_count = 0;
    uint64_t replace_end = logical_start + block_count;
    int replacement_emitted = 0;

    if (replace_end < logical_start) {
        errno = EINVAL;
        return -1;
    }

    for (uint32_t i = 0; i < old_count; ++i) {
        uint64_t l = le64toh(ext[i].logical_block);
        uint64_t phys = le64toh(ext[i].physical_block);
        uint32_t n = le32toh(ext[i].block_count);
        uint32_t flags = le32toh(ext[i].flags);
        uint64_t e = l + n;
        if (e <= logical_start || l >= replace_end) {
            if (extent_emit(rebuilt, &new_count, l, phys, n, flags) != 0)
                return -1;
            continue;
        }
        if (l < logical_start) {
            uint32_t prefix = (uint32_t)(logical_start - l);
            if (extent_emit(rebuilt, &new_count, l, phys, prefix, flags) != 0)
                return -1;
        }
        uint64_t overlap_start = l > logical_start ? l : logical_start;
        uint64_t overlap_end = e < replace_end ? e : replace_end;
        if (!replacement_emitted) {
            if (extent_emit(rebuilt, &new_count, logical_start, new_physical,
                            (uint32_t)block_count, INFS_EXTENT_NORMAL) != 0)
                return -1;
            replacement_emitted = 1;
        }
        free_run(vol, phys + (overlap_start - l), overlap_end - overlap_start);
        if (e > replace_end) {
            uint32_t suffix = (uint32_t)(e - replace_end);
            if (extent_emit(rebuilt, &new_count, replace_end,
                            phys + (replace_end - l), suffix, flags) != 0)
                return -1;
        }
    }
    if (!replacement_emitted) {
        errno = EIO;
        return -1;
    }

    memset(ext, 0, (size_t)INFS_MAX_FILE_EXTENTS * sizeof(*ext));
    memcpy(ext, rebuilt, (size_t)new_count * sizeof(*ext));
    p->extent_count = htole32(new_count);
    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)object;
    hdr->payload_size = htole32((uint32_t)(sizeof(*p) +
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
        return -1;
    data_block_checksum(data, &actual);
    if (memcmp(&expected, &actual, sizeof(expected)) != 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}
