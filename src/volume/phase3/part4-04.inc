// SPDX-License-Identifier: GPL-3.0-or-later
static int truncate_file_tx(struct infs_volume *vol, const char *path, off_t size)
{
    if (!vol->writable) {
        errno = EROFS;
        return -1;
    }
    if (size < 0) {
        errno = EINVAL;
        return -1;
    }
    struct infs_lookup lk;
    if (infs_lookup_path(vol, path, &lk) != 0)
        return -1;
    if (lk.type != INFS_OBJECT_FILE) {
        errno = EISDIR;
        return -1;
    }
    uint8_t object[INFS_BLOCK_SIZE];
    if (read_object(vol, lk.block, object) != 0)
        return -1;
    struct infs_file_payload_disk *p;
    struct infs_extent_disk *ext;
    if (file_validate_volume(vol, object, &p, &ext) != 0)
        return -1;

    uint64_t old_size = le64toh(p->stat.size);
    uint64_t target_blocks = ((uint64_t)size + INFS_BLOCK_SIZE - 1u) / INFS_BLOCK_SIZE;
    uint64_t have = file_allocated_blocks(p, ext);
    if (target_blocks > have) {
        if (file_ensure_blocks(vol, object, target_blocks) != 0)
            return -1;
        if (infs_object_finalize(object) != 0 ||
            file_validate_volume(vol, object, &p, &ext) != 0)
            return -1;
    } else if (target_blocks < have) {
        uint32_t count = le32toh(p->extent_count);
        uint32_t keep = 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t logical = le64toh(ext[i].logical_block);
            uint64_t physical = le64toh(ext[i].physical_block);
            uint32_t blocks = le32toh(ext[i].block_count);
            if (logical >= target_blocks) {
                free_run(vol, physical, blocks);
                continue;
            }
            uint64_t end = logical + blocks;
            if (end > target_blocks) {
                uint32_t retained = (uint32_t)(target_blocks - logical);
                free_run(vol, physical + retained, blocks - retained);
                ext[i].block_count = htole32(retained);
            }
            if (keep != i)
                ext[keep] = ext[i];
            ++keep;
        }
        if (keep < count)
            memset(&ext[keep], 0, (size_t)(count - keep) * sizeof(*ext));
        p->extent_count = htole32(keep);
        struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)object;
        hdr->payload_size = htole32((uint32_t)(sizeof(*p) +
            (size_t)keep * sizeof(*ext)));
    }

    if ((uint64_t)size < old_size && target_blocks > 0 && ((uint64_t)size % INFS_BLOCK_SIZE) != 0) {
        if (infs_object_finalize(object) != 0 ||
            file_validate_volume(vol, object, &p, &ext) != 0)
            return -1;
        struct infs_object_header_disk *file_hdr =
            (struct infs_object_header_disk *)object;
        uint64_t logical = target_blocks - 1u;
        uint64_t physical;
        if (file_map_block(p, ext, logical, &physical) != 0)
            return -1;
        uint8_t data[INFS_BLOCK_SIZE];
        if (read_block(vol, physical, data) != 0 ||
            data_block_verify(vol, p, file_hdr->object_id, logical, data) != 0)
            return -1;
        size_t tail = (size_t)((uint64_t)size % INFS_BLOCK_SIZE);
        memset(data + tail, 0, INFS_BLOCK_SIZE - tail);
        uint64_t replacement = 0, got = 0;
        if (alloc_run(vol, 1, &replacement, &got) != 0 || got != 1) {
            if (got != 1 && errno == 0) errno = EIO;
            return -1;
        }
        if (write_block(vol, replacement, data) != 0 ||
            file_replace_range(vol, object, logical, 1, replacement) != 0)
            return -1;
        struct infs_data_checksum_disk digest;
        data_block_checksum(data, &digest);
        if (checksum_set_range(vol, p, file_hdr->object_id, logical, &digest, 1) != 0)
            return -1;
    }

    p->stat.size = htole64((uint64_t)size);
    touch_mctime(&p->stat);
    if (write_object(vol, lk.block, object) != 0)
        return -1;
    return infs_volume_sync(vol);
}

int infs_truncate_file(struct infs_volume *vol, const char *path, off_t size)
{
    if (tx_begin(vol) != 0)
        return -1;
    int rc = truncate_file_tx(vol, path, size);
    if (rc != 0) {
        int saved = errno;
        tx_abort(vol);
        errno = saved;
    }
    return rc;
}
