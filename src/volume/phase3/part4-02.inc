// SPDX-License-Identifier: GPL-3.0-or-later
int64_t infs_read_file(struct infs_volume *vol, const char *path, void *buf,
                       size_t size, uint64_t offset)
{
    if (!vol || !path || (size && !buf))
        return INFS_STATUS_INVALID_ARGUMENT;
    if ((uint64_t)size > INT64_MAX) {
        return INFS_STATUS_OVERFLOW;
    }
    struct infs_lookup lk;
    if (infs_lookup_path(vol, path, &lk) != 0)
        return INFS_STATUS_ERROR;
    if (lk.type != INFS_OBJECT_FILE) {
        return INFS_STATUS_IS_DIRECTORY;
    }
    uint8_t object[INFS_BLOCK_SIZE];
    if (read_object(vol, lk.block, object) != 0)
        return INFS_STATUS_ERROR;
    struct infs_object_header_disk *file_hdr =
        (struct infs_object_header_disk *)object;
    struct infs_file_payload_disk *p;
    struct infs_extent_disk *ext;
    if (file_validate_volume(vol, object, &p, &ext) != 0)
        return INFS_STATUS_ERROR;
    uint64_t file_size = infs_le64_to_cpu(p->attributes.logical_size);
    uint64_t off = offset;
    if (off >= file_size)
        return 0;
    uint64_t remaining64 = file_size - off;
    size_t remaining = size;
    if ((uint64_t)remaining > remaining64)
        remaining = (size_t)remaining64;
    size_t done = 0;
    uint8_t data[INFS_BLOCK_SIZE];
    while (done < remaining) {
        uint64_t pos = off + done;
        uint64_t logical = pos / INFS_BLOCK_SIZE;
        size_t within = (size_t)(pos % INFS_BLOCK_SIZE);
        size_t chunk = INFS_BLOCK_SIZE - within;
        if (chunk > remaining - done)
            chunk = remaining - done;
        uint64_t physical;
        uint32_t flags;
        if (file_map_block(p, ext, logical, &physical, &flags) != 0)
            return INFS_STATUS_ERROR;
        if (flags == INFS_EXTENT_HOLE) {
            memset((uint8_t *)buf + done, 0, chunk);
        } else {
            if (read_block(vol, physical, data) != 0 ||
                data_block_verify(vol, p, file_hdr->object_id, logical,
                                  data) != 0)
                return INFS_STATUS_ERROR;
            memcpy((uint8_t *)buf + done, data + within, chunk);
        }
        done += chunk;
    }
    return (int64_t)done;
}
