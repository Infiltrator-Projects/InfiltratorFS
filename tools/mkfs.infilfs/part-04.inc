// SPDX-License-Identifier: GPL-3.0-or-later
    if (infs_pwrite_full(fd, bitmap, bitmap_alloc,
                         (off_t)(bitmap_start * INFS_BLOCK_SIZE)) != 0) {
        perror("write bitmap");
        free(bitmap);
        close(fd);
        return 1;
    }

    if (infs_metadata_page_init(block, INFS_INDEX_PAGE_MAGIC, index_id, 1) !=
        INFS_STATUS_OK) {
        fprintf(stderr, "encode object index page failed\n");
        free(bitmap);
        close(fd);
        return 1;
    }
    struct infs_metadata_page_disk *index_page =
        (struct infs_metadata_page_disk *)block;
    struct infs_index_entry_disk *ie =
        (struct infs_index_entry_disk *)(index_page + 1);
    memset(ie, 0, sizeof(*ie));
    memcpy(ie->object_id, root_id, 16);
    ie->object_block = infs_cpu_to_le64(root_block);
    ie->object_type = infs_cpu_to_le16(INFS_OBJECT_DIRECTORY);
    index_page->entry_count = infs_cpu_to_le32(1);
    index_page->bytes_used = infs_cpu_to_le32(sizeof(*ie));
    if (infs_metadata_page_finalize(block) != INFS_STATUS_OK ||
        infs_pwrite_full(fd, block, sizeof(block),
                         (off_t)(index_page_block * INFS_BLOCK_SIZE)) != 0) {
        perror("write object index page");
        free(bitmap);
        close(fd);
        return 1;
    }

    if (infs_encode_paged_object_index(block, index_id, 1) != INFS_STATUS_OK) {
        fprintf(stderr, "encode object index failed\n");
        free(bitmap);
        close(fd);
        return 1;
    }
    struct infs_object_header_disk *ih = (struct infs_object_header_disk *)block;
    struct infs_index_payload_disk *ip =
        (struct infs_index_payload_disk *)(block + sizeof(*ih));
    uint64_t *page_pointer = (uint64_t *)(ip + 1);
    ip->entry_count = infs_cpu_to_le32(1);
    ip->reserved = infs_cpu_to_le32(1);
    page_pointer[0] = infs_cpu_to_le64(index_page_block);
    ih->payload_size = infs_cpu_to_le32(sizeof(*ip) + sizeof(*page_pointer));
    if (infs_object_finalize(block) != INFS_STATUS_OK ||
        infs_pwrite_full(fd, block, sizeof(block),
                         (off_t)(index_block * INFS_BLOCK_SIZE)) != 0) {
        perror("write object index");
        free(bitmap);
        close(fd);
        return 1;
    }

    errno = 0;
    int64_t root_time_ns = infs_current_time_ns();
    if (root_time_ns == 0 && errno != 0) {
        perror("clock_gettime");
        free(bitmap);
        close(fd);
        return 1;
    }
    if (infs_encode_paged_root_directory(
            block, root_id, 1, 0755u,
            (uint32_t)getuid(), (uint32_t)getgid(), root_time_ns) !=
            INFS_STATUS_OK ||
        infs_pwrite_full(fd, block, sizeof(block),
                         (off_t)(root_block * INFS_BLOCK_SIZE)) != 0) {
        perror("write root directory");
        free(bitmap);
        close(fd);
        return 1;
    }

    /* Make every structure referenced by the checkpoint durable first. */
    if (fsync(fd) != 0) {
        perror("fsync metadata");
        free(bitmap);
        close(fd);
        return 1;
    }

    if (infs_encode_superblock(block, &sb) != INFS_STATUS_OK) {
        fprintf(stderr, "encode checkpoint failed\n");
        free(bitmap);
        close(fd);
        return 1;
    }
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        off_t off = (off_t)(checkpoints[i] * INFS_BLOCK_SIZE);
        if (infs_pwrite_full(fd, block, sizeof(block), off) != 0) {
            perror("write checkpoint");
            free(bitmap);
            close(fd);
            return 1;
        }
    }
    if (fsync(fd) != 0) {
        perror("fsync checkpoints");
        free(bitmap);
        close(fd);
        return 1;
    }

    char uuid_text[37];
    infs_uuid_to_string(fs_uuid, uuid_text);
    printf("InfiltratorFS formatted successfully\n");
    printf("  Implementation: %s\n", INFILFS_IMPLEMENTATION_VERSION);
    printf("  Format:          %u.%u\n", INFS_FORMAT_MAJOR, INFS_FORMAT_MINOR);
    printf("  UUID:            %s\n", uuid_text);
    printf("  Label:           %s\n", (char *)sb.label);
    printf("  Block size:      %u bytes\n", INFS_BLOCK_SIZE);
    printf("  Total blocks:    %" PRIu64 "\n", total_blocks);
    printf("  Free blocks:     %" PRIu64 "\n", total_blocks - used_blocks);
    printf("  Bitmap blocks:   %" PRIu64 "\n", bitmap_blocks);
    printf("  Object index:    block %" PRIu64 "\n", index_block);
    printf("  Index page:      block %" PRIu64 "\n", index_page_block);
    printf("  Root block:      block %" PRIu64 "\n", root_block);
    printf("  Checkpoints:     %" PRIu64 ", %" PRIu64 ", %" PRIu64 "\n",
           checkpoints[0], checkpoints[1], checkpoints[2]);

    free(bitmap);
    close(fd);
    return 0;
}
