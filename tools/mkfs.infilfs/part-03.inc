// SPDX-License-Identifier: GPL-3.0-or-later

    bitmap_set(bitmap, checkpoints[0]);
    bitmap_set(bitmap, checkpoints[1]);
    bitmap_set(bitmap, checkpoints[2]);
    for (uint64_t b = bitmap_start; b < bitmap_start + bitmap_blocks; ++b)
        bitmap_set(bitmap, b);
    bitmap_set(bitmap, index_block);
    bitmap_set(bitmap, root_block);
    bitmap_set(bitmap, index_page_block);

    for (uint64_t b = total_blocks; b < bitmap_blocks * INFS_BLOCK_SIZE * 8ull; ++b)
        bitmap_set(bitmap, b);

    uint64_t used_blocks = 0;
    for (uint64_t b = 0; b < total_blocks; ++b)
        if (bitmap_get(bitmap, b))
            ++used_blocks;

    uint8_t fs_uuid[16];
    uint8_t root_id[16];
    uint8_t index_id[16];
    if (generate_id(fs_uuid, NULL, NULL) != 0 ||
        generate_id(root_id, fs_uuid, NULL) != 0 ||
        generate_id(index_id, fs_uuid, root_id) != 0) {
        perror("getrandom");
        free(bitmap);
        close(fd);
        return 1;
    }

    struct infs_superblock_disk sb;
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, INFS_MAGIC, 8);
    sb.format_major = infs_cpu_to_le16(INFS_FORMAT_MAJOR);
    sb.format_minor = infs_cpu_to_le16(INFS_FORMAT_MINOR);
    sb.header_size = infs_cpu_to_le16(sizeof(sb));
    sb.block_shift = infs_cpu_to_le16(INFS_BLOCK_SHIFT);
    sb.checksum_type = infs_cpu_to_le32(INFS_CHECKSUM_CRC64_ECMA);
    sb.generation = infs_cpu_to_le64(1);
    sb.total_blocks = infs_cpu_to_le64(total_blocks);
    sb.free_blocks = infs_cpu_to_le64(total_blocks - used_blocks);
    sb.bitmap_start_block = infs_cpu_to_le64(bitmap_start);
    sb.bitmap_block_count = infs_cpu_to_le64(bitmap_blocks);
    sb.object_index_block = infs_cpu_to_le64(index_block);
    sb.root_object_block = infs_cpu_to_le64(root_block);
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        sb.checkpoint_block[i] = infs_cpu_to_le64(checkpoints[i]);
    memcpy(sb.filesystem_uuid, fs_uuid, sizeof(fs_uuid));
    memcpy(sb.root_object_id, root_id, sizeof(root_id));
    sb.compat_flags = infs_cpu_to_le64(INFS_KNOWN_COMPAT_FLAGS);
    sb.ro_compat_flags = infs_cpu_to_le64(INFS_KNOWN_RO_COMPAT_FLAGS);
    sb.incompat_flags = infs_cpu_to_le64(
        INFS_INCOMPAT_UTF8_NAMES | INFS_INCOMPAT_SPARSE_EXTENTS |
        INFS_INCOMPAT_INLINE_DATA | INFS_INCOMPAT_SHARED_EXTENTS |
        INFS_INCOMPAT_PAGED_METADATA | INFS_INCOMPAT_SYMBOLIC_LINKS);
    memcpy(sb.label, label, strlen(label));

    /* Invalidate any previous InfiltratorFS checkpoints before constructing
     * the new metadata. If formatting is interrupted before final publication,
     * the target is deliberately unmountable rather than half-formatted. */
    uint8_t block[INFS_BLOCK_SIZE] = {0};
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        off_t off = (off_t)(checkpoints[i] * INFS_BLOCK_SIZE);
        if (infs_pwrite_full(fd, block, sizeof(block), off) != 0) {
            perror("invalidate checkpoint");
            free(bitmap);
            close(fd);
            return 1;
        }
    }
    if (fsync(fd) != 0) {
        perror("fsync checkpoint invalidation");
        free(bitmap);
        close(fd);
        return 1;
    }
