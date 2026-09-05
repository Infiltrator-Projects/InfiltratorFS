// SPDX-License-Identifier: GPL-3.0-or-later
    for (uint64_t i = 0; i < leaf_count; ++i) {
        uint64_t first_bit = i * INFS_ALLOCATION_BITS_PER_LEAF;
        uint64_t remaining = total_blocks - first_bit;
        uint32_t valid_bits = (uint32_t)(
            remaining > INFS_ALLOCATION_BITS_PER_LEAF ?
            INFS_ALLOCATION_BITS_PER_LEAF : remaining);
        uint32_t bytes = (valid_bits + 7u) / 8u;
        size_t offset = (size_t)i * INFS_ALLOCATION_PAGE_DATA_SIZE;
        if (write_allocation_page_fd(
                fd, leaf_start + i, INFS_ALLOCATION_LEAF_PAGE_MAGIC,
                i, 0u, valid_bits, bitmap + offset, bytes) != 0) {
            perror("write allocation leaf");
            free(bitmap);
            close(fd);
            return 1;
        }
    }

    for (uint64_t i = 0; i < level1_count; ++i) {
        uint64_t first_leaf = i * INFS_ALLOCATION_TREE_FANOUT;
        uint32_t entries = (uint32_t)(
            leaf_count - first_leaf > INFS_ALLOCATION_TREE_FANOUT ?
            INFS_ALLOCATION_TREE_FANOUT : leaf_count - first_leaf);
        uint64_t pointers[INFS_ALLOCATION_TREE_FANOUT];
        for (uint32_t j = 0; j < entries; ++j)
            pointers[j] = infs_cpu_to_le64(leaf_start + first_leaf + j);
        if (write_allocation_page_fd(
                fd, level1_start + i, INFS_ALLOCATION_BRANCH_PAGE_MAGIC,
                i, 1u, entries, pointers,
                entries * sizeof(uint64_t)) != 0) {
            perror("write allocation level-1 branch");
            free(bitmap);
            close(fd);
            return 1;
        }
    }

    for (uint64_t i = 0; i < level2_count; ++i) {
        uint64_t first_l1 = i * INFS_ALLOCATION_TREE_FANOUT;
        uint32_t entries = (uint32_t)(
            level1_count - first_l1 > INFS_ALLOCATION_TREE_FANOUT ?
            INFS_ALLOCATION_TREE_FANOUT : level1_count - first_l1);
        uint64_t pointers[INFS_ALLOCATION_TREE_FANOUT];
        for (uint32_t j = 0; j < entries; ++j)
            pointers[j] = infs_cpu_to_le64(level1_start + first_l1 + j);
        if (write_allocation_page_fd(
                fd, level2_start + i, INFS_ALLOCATION_BRANCH_PAGE_MAGIC,
                i, 2u, entries, pointers,
                entries * sizeof(uint64_t)) != 0) {
            perror("write allocation level-2 branch");
            free(bitmap);
            close(fd);
            return 1;
        }
    }

    {
        uint64_t pointers[INFS_ALLOCATION_TREE_FANOUT];
        for (uint32_t i = 0; i < (uint32_t)level2_count; ++i)
            pointers[i] = infs_cpu_to_le64(level2_start + i);
        if (write_allocation_page_fd(
                fd, allocation_root, INFS_ALLOCATION_BRANCH_PAGE_MAGIC,
                0u, INFS_ALLOCATION_TREE_ROOT_LEVEL,
                (uint32_t)level2_count, pointers,
                (uint32_t)level2_count * sizeof(uint64_t)) != 0) {
            perror("write allocation root");
            free(bitmap);
            close(fd);
            return 1;
        }
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

    if (infs_encode_tree_object_index(
            block, index_id, 1, index_page_block, 1) != INFS_STATUS_OK) {
        fprintf(stderr, "encode object index failed\n");
        free(bitmap);
        close(fd);
        return 1;
    }
    if (infs_pwrite_full(fd, block, sizeof(block),
                         (off_t)(index_block * INFS_BLOCK_SIZE)) != 0) {
        perror("write object index");
        free(bitmap);
        close(fd);
        return 1;
    }

    struct infs_timestamp root_time = {0};
    if (infs_current_time(&root_time) != 0) {
        perror("clock_gettime");
        free(bitmap);
        close(fd);
        return 1;
    }
    uint32_t root_uid = 0;
    uint32_t root_gid = 0;
    resolve_root_owner(&root_uid, &root_gid);
    if (infs_encode_tree_root_directory(
            block, root_id, 1, 0755u,
            root_uid, root_gid, &root_time) !=
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
    printf("  Root owner:      %" PRIu32 ":%" PRIu32 "\n", root_uid, root_gid);
    printf("  Block size:      %u bytes\n", INFS_BLOCK_SIZE);
    printf("  Total blocks:    %" PRIu64 "\n", total_blocks);
    printf("  Free blocks:     %" PRIu64 "\n", total_blocks - used_blocks);
    printf("  Allocation root: block %" PRIu64 "\n", allocation_root);
    printf("  Allocation leaves: %" PRIu64 "\n", leaf_count);
    printf("  Object index:    block %" PRIu64 "\n", index_block);
    printf("  Index tree root: block %" PRIu64 "\n", index_page_block);
    printf("  Root block:      block %" PRIu64 "\n", root_block);
    printf("  Checkpoints:     %" PRIu64 ", %" PRIu64 ", %" PRIu64 "\n",
           checkpoints[0], checkpoints[1], checkpoints[2]);

    free(bitmap);
    close(fd);
    return 0;
}
