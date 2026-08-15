// SPDX-License-Identifier: GPL-3.0-or-later
    if (infs_pwrite_full(fd, bitmap, bitmap_alloc,
                         (off_t)(bitmap_start * INFS_BLOCK_SIZE)) != 0) {
        perror("write bitmap");
        free(bitmap);
        close(fd);
        return 1;
    }

    if (infs_encode_object_index(block, index_id, 1) != 0) {
        perror("encode object index");
        free(bitmap);
        close(fd);
        return 1;
    }
    struct infs_object_header_disk *ih = (struct infs_object_header_disk *)block;
    struct infs_index_payload_disk *ip =
        (struct infs_index_payload_disk *)(block + sizeof(*ih));
    struct infs_index_entry_disk *ie = (struct infs_index_entry_disk *)(ip + 1);
    ip->entry_count = htole32(1);
    memcpy(ie->object_id, root_id, 16);
    ie->object_block = htole64(root_block);
    ie->object_type = htole16(INFS_OBJECT_DIRECTORY);
    ih->payload_size = htole32(sizeof(*ip) + sizeof(*ie));
    if (infs_object_finalize(block) != 0 ||
        infs_pwrite_full(fd, block, sizeof(block),
                         (off_t)(index_block * INFS_BLOCK_SIZE)) != 0) {
        perror("write object index");
        free(bitmap);
        close(fd);
        return 1;
    }

    if (infs_encode_root_directory(block, root_id, 1,
                                   (uint32_t)getuid(), (uint32_t)getgid(),
                                   current_time_ns()) != 0 ||
        infs_pwrite_full(fd, block, sizeof(block),
                         (off_t)(root_block * INFS_BLOCK_SIZE)) != 0) {
        perror("write root directory");
        free(bitmap);
        close(fd);
        return 1;
    }

    if (fsync(fd) != 0) {
        perror("fsync");
        free(bitmap);
        close(fd);
        return 1;
    }

    char uuid_text[37];
    infs_uuid_to_string(fs_uuid, uuid_text);
    printf("InfiltratorFS formatted successfully\n");
    printf("  Format:          %u.%u\n", INFS_FORMAT_MAJOR, INFS_FORMAT_MINOR);
    printf("  UUID:            %s\n", uuid_text);
    printf("  Label:           %s\n", (char *)sb.label);
    printf("  Block size:      %u bytes\n", INFS_BLOCK_SIZE);
    printf("  Total blocks:    %" PRIu64 "\n", total_blocks);
    printf("  Free blocks:     %" PRIu64 "\n", total_blocks - used_blocks);
    printf("  Bitmap blocks:   %" PRIu64 "\n", bitmap_blocks);
    printf("  Object index:    block %" PRIu64 "\n", index_block);
    printf("  Root block:      %" PRIu64 "\n", root_block);
    printf("  Checkpoints:     %" PRIu64 ", %" PRIu64 ", %" PRIu64 "\n",
           checkpoints[0], checkpoints[1], checkpoints[2]);

    free(bitmap);
    close(fd);
    return 0;
}
