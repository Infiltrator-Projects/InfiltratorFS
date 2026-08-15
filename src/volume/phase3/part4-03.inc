// SPDX-License-Identifier: GPL-3.0-or-later
static ssize_t write_file_tx(struct infs_volume *vol, const char *path,
                        const void *buf, size_t size, off_t offset)
{
    if (!vol->writable) {
        errno = EROFS;
        return -1;
    }
    if (offset < 0 || (uint64_t)offset > UINT64_MAX - size) {
        errno = EINVAL;
        return -1;
    }
    if (size == 0)
        return 0;
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
    uint64_t end = (uint64_t)offset + size;
    uint64_t needed_blocks = (end + INFS_BLOCK_SIZE - 1u) / INFS_BLOCK_SIZE;
    if (file_ensure_blocks(vol, object, needed_blocks) != 0)
        return -1;
    if (infs_object_finalize(object) != 0 ||
        file_validate_volume(vol, object, &p, &ext) != 0)
        return -1;

    struct infs_object_header_disk *file_hdr =
        (struct infs_object_header_disk *)object;
    size_t done = 0;
    uint8_t data[INFS_BLOCK_SIZE];
    while (done < size) {
        uint64_t pos = (uint64_t)offset + done;
        uint64_t logical = pos / INFS_BLOCK_SIZE;
        size_t within = (size_t)(pos % INFS_BLOCK_SIZE);
        size_t chunk = INFS_BLOCK_SIZE - within;
        if (chunk > size - done)
            chunk = size - done;
        uint64_t physical;
        if (file_map_block(p, ext, logical, &physical) != 0)
            return -1;

        int full_overwrite = within == 0 && chunk == INFS_BLOCK_SIZE;
        if (!full_overwrite) {
            if (read_block(vol, physical, data) != 0 ||
                data_block_verify(vol, p, file_hdr->object_id, logical, data) != 0)
                return -1;
        } else {
            memset(data, 0, sizeof(data));
        }
        memcpy(data + within, (const uint8_t *)buf + done, chunk);

        if (bitmap_get(vol->tx_base_bitmap, physical)) {
            uint64_t replacement = 0, got = 0;
            if (alloc_run(vol, 1, &replacement, &got) != 0)
                return -1;
            if (got != 1) {
                errno = EIO;
                return -1;
            }
            if (write_block(vol, replacement, data) != 0 ||
                file_replace_range(vol, object, logical, 1, replacement) != 0)
                return -1;
            physical = replacement;
        } else if (write_block(vol, physical, data) != 0) {
            return -1;
        }

        struct infs_data_checksum_disk digest;
        data_block_checksum(data, &digest);
        if (checksum_set_range(vol, p, file_hdr->object_id, logical, &digest, 1) != 0)
            return -1;
        done += chunk;
    }
    if (end > le64toh(p->stat.size))
        p->stat.size = htole64(end);
    touch_mctime(&p->stat);
    if (write_object(vol, lk.block, object) != 0 || infs_volume_sync(vol) != 0)
        return -1;
    return (ssize_t)done;
}

ssize_t infs_write_file(struct infs_volume *vol, const char *path,
                        const void *buf, size_t size, off_t offset)
{
    if (size == 0)
        return 0;
    if (tx_begin(vol) != 0)
        return -1;
    ssize_t rc = write_file_tx(vol, path, buf, size, offset);
    if (rc < 0) {
        int saved = errno;
        tx_abort(vol);
        errno = saved;
    }
    return rc;
}
