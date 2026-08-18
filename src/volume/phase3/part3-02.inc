// SPDX-License-Identifier: GPL-3.0-or-later
static int file_ensure_logical_blocks(struct infs_volume *vol,
                                      uint8_t block[INFS_BLOCK_SIZE],
                                      uint64_t needed)
{
    struct infs_file_payload_disk *p;
    struct infs_extent_disk *ext;
    infs_status status = file_validate_volume(vol, block, &p, &ext);
    if (status != INFS_STATUS_OK)
        return status;
    uint64_t have = file_logical_blocks(p, ext);
    if (needed <= have)
        return INFS_STATUS_OK;

    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)block;
    uint32_t count = infs_le32_to_cpu(p->extent_count);
    const size_t max_extents = (INFS_BLOCK_SIZE - sizeof(*hdr) - sizeof(*p)) /
                               sizeof(struct infs_extent_disk);

    while (have < needed) {
        uint64_t remaining = needed - have;
        if (count &&
            infs_le32_to_cpu(ext[count - 1u].flags) == INFS_EXTENT_HOLE) {
            uint32_t old_count =
                infs_le32_to_cpu(ext[count - 1u].block_count);
            uint64_t capacity = UINT32_MAX - (uint64_t)old_count;
            uint64_t add = remaining < capacity ? remaining : capacity;
            if (add) {
                ext[count - 1u].block_count =
                    infs_cpu_to_le32(old_count + (uint32_t)add);
                have += add;
                continue;
            }
        }
        if (count >= max_extents)
            return INFS_STATUS_FILE_TOO_LARGE;
        uint32_t chunk = remaining > UINT32_MAX ?
            UINT32_MAX : (uint32_t)remaining;
        ext[count].logical_block = infs_cpu_to_le64(have);
        ext[count].physical_block = 0;
        ext[count].block_count = infs_cpu_to_le32(chunk);
        ext[count].flags = infs_cpu_to_le32(INFS_EXTENT_HOLE);
        ++count;
        p->extent_count = infs_cpu_to_le32(count);
        have += chunk;
    }
    hdr->payload_size = infs_cpu_to_le32((uint32_t)(sizeof(*p) +
        (size_t)count * sizeof(struct infs_extent_disk)));
    return INFS_STATUS_OK;
}

static int file_map_block(const struct infs_file_payload_disk *p,
                          const struct infs_extent_disk *ext,
                          uint64_t logical, uint64_t *physical,
                          uint32_t *flags_out)
{
    uint32_t count = infs_le32_to_cpu(p->extent_count);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t l = infs_le64_to_cpu(ext[i].logical_block);
        uint32_t n = infs_le32_to_cpu(ext[i].block_count);
        if (logical >= l && logical < l + n) {
            uint32_t flags = infs_le32_to_cpu(ext[i].flags);
            if (physical) {
                *physical = flags == INFS_EXTENT_HOLE ? 0 :
                    infs_le64_to_cpu(ext[i].physical_block) + (logical - l);
            }
            if (flags_out)
                *flags_out = flags;
            return INFS_STATUS_OK;
        }
    }
    return INFS_STATUS_CORRUPT;
}
