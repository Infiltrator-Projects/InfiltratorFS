// SPDX-License-Identifier: GPL-3.0-or-later
int infs_volume_sync(struct infs_volume *vol)
{
    if (!vol || vol->fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (!vol->writable)
        return 0;
    if (!vol->tx_active)
        return fsync(vol->fd);

    const uint64_t old_bitmap_start = le64toh(vol->tx_base_sb.bitmap_start_block);
    const uint64_t bitmap_blocks = le64toh(vol->tx_base_sb.bitmap_block_count);
    if (tx_defer_range(vol, old_bitmap_start, bitmap_blocks) != 0)
        return -1;

    uint64_t new_bitmap_start = 0;
    if (alloc_exact_run(vol, bitmap_blocks, &new_bitmap_start) != 0)
        return -1;

    /* Deferred frees remain reserved until every new metadata/data write is on
     * stable storage. They become reusable only in the new bitmap image. */
    for (size_t i = 0; i < vol->tx_deferred_count; ++i) {
        uint64_t start = vol->tx_deferred[i].start;
        uint64_t count = vol->tx_deferred[i].count;
        uint64_t total = le64toh(vol->sb.total_blocks);
        for (uint64_t b = start; b < start + count && b < total; ++b)
            bitmap_set(vol->bitmap, b, 0);
    }
    for (uint64_t i = 0; i < bitmap_blocks; ++i)
        bitmap_set(vol->bitmap, new_bitmap_start + i, 1);
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        bitmap_set(vol->bitmap, le64toh(vol->sb.checkpoint_block[i]), 1);
    for (uint64_t b = le64toh(vol->sb.total_blocks);
         b < (uint64_t)vol->bitmap_bytes * 8u; ++b)
        bitmap_set(vol->bitmap, b, 1);

    vol->sb.bitmap_start_block = htole64(new_bitmap_start);
    vol->sb.generation = htole64(le64toh(vol->tx_base_sb.generation) + 1u);
    vol->sb.free_blocks = htole64(bitmap_free_count(vol));

    /* Ordered transaction protocol:
     *   1. new metadata/data already written to unreachable blocks;
     *   2. flush those blocks;
     *   3. write and flush a new bitmap image;
     *   4. publish one checksummed checkpoint (commit point);
     *   5. replicate that checkpoint to the other physical copies.
     * A crash before step 4 selects the old generation. A crash after step 4
     * selects the new generation and a writable open heals the other copies. */
    if (fsync(vol->fd) != 0)
        return -1;
    phase2_failpoint("before-bitmap");

    if (infs_pwrite_full(vol->fd, vol->bitmap, vol->bitmap_bytes,
                         (off_t)(new_bitmap_start * INFS_BLOCK_SIZE)) != 0)
        return -1;
    if (fsync(vol->fd) != 0)
        return -1;
    phase2_failpoint("after-bitmap");

    uint8_t sb_block[INFS_BLOCK_SIZE];
    if (infs_encode_superblock(sb_block, &vol->sb) != 0)
        return -1;
    uint64_t generation = le64toh(vol->sb.generation);
    unsigned first = (unsigned)(generation % INFS_CHECKPOINT_COUNT);
    uint64_t first_block = le64toh(vol->sb.checkpoint_block[first]);
    if (write_block(vol, first_block, sb_block) != 0)
        return -1;
    if (fsync(vol->fd) != 0)
        return -1;

    /* The transaction is committed from this point onward. */
    free(vol->tx_base_bitmap);
    vol->tx_base_bitmap = NULL;
    vol->tx_deferred_count = 0;
    vol->tx_active = 0;
    phase2_failpoint("after-checkpoint");

    int replica_error = 0;
    int saved_errno = 0;
    for (unsigned n = 1; n < INFS_CHECKPOINT_COUNT; ++n) {
        unsigned i = (first + n) % INFS_CHECKPOINT_COUNT;
        uint64_t b = le64toh(vol->sb.checkpoint_block[i]);
        if (write_block(vol, b, sb_block) != 0) {
            if (!replica_error)
                saved_errno = errno;
            replica_error = 1;
        }
    }
    if (!replica_error && fsync(vol->fd) != 0) {
        saved_errno = errno;
        replica_error = 1;
    }
    if (replica_error) {
        errno = saved_errno ? saved_errno : EIO;
        return -1;
    }
    return 0;
}
