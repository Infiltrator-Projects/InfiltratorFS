// SPDX-License-Identifier: GPL-3.0-or-later
static int file_ensure_blocks(struct infs_volume *vol, uint8_t block[INFS_BLOCK_SIZE],
                              uint64_t needed)
{
    struct infs_file_payload_disk *p;
    struct infs_extent_disk *ext;
    if (file_validate_volume(vol, block, &p, &ext) != 0)
        return -1;
    uint64_t have = file_allocated_blocks(p, ext);
    if (needed <= have)
        return 0;

    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
    uint32_t count = infs_le32_to_cpu(p->extent_count);
    const size_t max_extents = (INFS_BLOCK_SIZE - sizeof(*hdr) - sizeof(*p)) /
                               sizeof(struct infs_extent_disk);

    while (have < needed) {
        uint64_t start, got;
        if (alloc_run(vol, needed - have, &start, &got) != 0)
            return -1;
        if (count &&
            infs_le64_to_cpu(ext[count - 1u].physical_block) + infs_le32_to_cpu(ext[count - 1u].block_count) == start) {
            uint64_t new_count = (uint64_t)infs_le32_to_cpu(ext[count - 1u].block_count) + got;
            if (new_count > UINT32_MAX) {
                free_run(vol, start, got);
                errno = EFBIG;
                return -1;
            }
            ext[count - 1u].block_count = infs_cpu_to_le32((uint32_t)new_count);
        } else {
            if (count >= max_extents) {
                free_run(vol, start, got);
                errno = EFBIG;
                return -1;
            }
            ext[count].logical_block = infs_cpu_to_le64(have);
            ext[count].physical_block = infs_cpu_to_le64(start);
            ext[count].block_count = infs_cpu_to_le32((uint32_t)got);
            ext[count].flags = infs_cpu_to_le32(INFS_EXTENT_NORMAL);
            ++count;
            p->extent_count = infs_cpu_to_le32(count);
        }
        if (checksum_set_zero_range(vol, p, hdr->object_id, have, got) != 0)
            return -1;
        have += got;
    }
    hdr->payload_size = infs_cpu_to_le32((uint32_t)(sizeof(*p) +
        (size_t)count * sizeof(struct infs_extent_disk)));
    return 0;
}

static int file_map_block(const struct infs_file_payload_disk *p,
                          const struct infs_extent_disk *ext,
                          uint64_t logical, uint64_t *physical)
{
    uint32_t count = infs_le32_to_cpu(p->extent_count);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t l = infs_le64_to_cpu(ext[i].logical_block);
        uint32_t n = infs_le32_to_cpu(ext[i].block_count);
        if (logical >= l && logical < l + n) {
            *physical = infs_le64_to_cpu(ext[i].physical_block) + (logical - l);
            return 0;
        }
    }
    errno = EIO;
    return -1;
}
