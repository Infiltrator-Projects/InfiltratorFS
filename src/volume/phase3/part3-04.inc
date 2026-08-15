// SPDX-License-Identifier: GPL-3.0-or-later
int infs_volume_open(struct infs_volume *vol, const char *path, int writable)
{
    if (!vol || !path) {
        errno = EINVAL;
        return -1;
    }
    memset(vol, 0, sizeof(*vol));
    vol->fd = -1;
    vol->writable = writable != 0;
    vol->fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (vol->fd < 0)
        return -1;
    int is_block = 0;
    if (infs_get_size_bytes(vol->fd, &vol->size_bytes, &is_block) != 0)
        goto fail;
    (void)is_block;
    if (infs_read_best_superblock(vol->fd, vol->size_bytes, &vol->sb, NULL) != 0)
        goto fail;
    if (le16toh(vol->sb.format_minor) != INFS_FORMAT_MINOR) {
        errno = EOPNOTSUPP;
        goto fail;
    }

    uint64_t bitmap_start = le64toh(vol->sb.bitmap_start_block);
    uint64_t bitmap_blocks = le64toh(vol->sb.bitmap_block_count);
    uint64_t total = le64toh(vol->sb.total_blocks);
    if (bitmap_start == 0 || bitmap_blocks == 0 || bitmap_start >= total ||
        bitmap_blocks > total - bitmap_start ||
        bitmap_blocks > SIZE_MAX / INFS_BLOCK_SIZE) {
        errno = EIO;
        goto fail;
    }
    vol->bitmap_bytes = (size_t)(bitmap_blocks * INFS_BLOCK_SIZE);
    vol->bitmap = malloc(vol->bitmap_bytes);
    if (!vol->bitmap)
        goto fail;
    if (infs_pread_full(vol->fd, vol->bitmap, vol->bitmap_bytes,
                        (off_t)(bitmap_start * INFS_BLOCK_SIZE)) != 0)
        goto fail;

    uint64_t free_count = 0;
    for (uint64_t b = 0; b < total; ++b) {
        if (!bitmap_get(vol->bitmap, b))
            ++free_count;
    }
    if (free_count != le64toh(vol->sb.free_blocks)) {
        errno = EIO;
        goto fail;
    }
    for (uint64_t b = total; b < (uint64_t)vol->bitmap_bytes * 8u; ++b) {
        if (!bitmap_get(vol->bitmap, b)) {
            errno = EIO;
            goto fail;
        }
    }
    for (uint64_t b = bitmap_start; b < bitmap_start + bitmap_blocks; ++b) {
        if (!bitmap_get(vol->bitmap, b)) {
            errno = EIO;
            goto fail;
        }
    }

    uint64_t critical[] = {
        le64toh(vol->sb.checkpoint_block[0]),
        le64toh(vol->sb.checkpoint_block[1]),
        le64toh(vol->sb.checkpoint_block[2]),
        le64toh(vol->sb.object_index_block),
        le64toh(vol->sb.root_object_block)
    };
    for (size_t i = 0; i < sizeof(critical) / sizeof(critical[0]); ++i) {
        if (critical[i] >= total || !bitmap_get(vol->bitmap, critical[i])) {
            errno = EIO;
            goto fail;
        }
    }

    uint8_t object[INFS_BLOCK_SIZE];
    if (read_object(vol, le64toh(vol->sb.root_object_block), object) != 0 ||
        dir_validate(object, NULL) != 0)
        goto fail;
    struct infs_object_header_disk *root_hdr = (struct infs_object_header_disk *)object;
    if (memcmp(root_hdr->object_id, vol->sb.root_object_id, 16) != 0 ||
        !id_is_zero(root_hdr->parent_id)) {
        errno = EIO;
        goto fail;
    }
    if (validate_index_entries(vol) != 0 || validate_integrity_metadata(vol) != 0)
        goto fail;
    if (vol->writable && heal_checkpoints(vol) != 0)
        goto fail;
    return 0;

fail:
    {
        int saved = errno;
        free(vol->bitmap);
        vol->bitmap = NULL;
        if (vol->fd >= 0)
            close(vol->fd);
        vol->fd = -1;
        errno = saved;
        return -1;
    }
}

void infs_volume_close(struct infs_volume *vol)
{
    if (!vol)
        return;
    tx_abort(vol);
    free(vol->tx_base_bitmap);
    vol->tx_base_bitmap = NULL;
    free(vol->tx_deferred);
    vol->tx_deferred = NULL;
    vol->tx_deferred_count = 0;
    vol->tx_deferred_capacity = 0;
    free(vol->bitmap);
    vol->bitmap = NULL;
    if (vol->fd >= 0)
        close(vol->fd);
    vol->fd = -1;
}
