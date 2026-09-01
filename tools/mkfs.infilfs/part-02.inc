// SPDX-License-Identifier: GPL-3.0-or-later
int main(int argc, char **argv)
{
    int force = 0;
    int wipe_only = 0;
    const char *label = "InfiltratorFS";
    const char *path = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--force") == 0) {
            force = 1;
        } else if (strcmp(argv[i], "--wipe-only") == 0) {
            wipe_only = 1;
        } else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            label = argv[++i];
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else if (!path) {
            path = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!path) {
        usage(argv[0]);
        return 2;
    }
    if (strlen(label) >= INFS_LABEL_MAX) {
        fprintf(stderr, "Label is too long (maximum %u bytes).\n",
                INFS_LABEL_MAX - 1u);
        return 2;
    }
    if (!infs_utf8_validate(label, strlen(label))) {
        fprintf(stderr, "Label must be valid UTF-8.\n");
        return 2;
    }

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    uint64_t size_bytes = 0;
    int is_block = 0;
    if (infs_get_size_bytes(fd, &size_bytes, &is_block) != 0) {
        perror("size");
        close(fd);
        return 1;
    }

    if (is_block && !force) {
        fprintf(stderr, "Refusing to format a block device without --force.\n");
        close(fd);
        return 1;
    }
    if (is_block) {
        struct stat original;
        if (fstat(fd, &original) != 0 || !S_ISBLK(original.st_mode)) {
            fprintf(stderr,
                    "Refusing to format because the block target identity changed.\n");
            close(fd);
            return 1;
        }

        int in_use = block_device_in_use(fd);
        if (in_use > 0) {
            fprintf(stderr,
                    "Refusing to format an active block device or a disk with an active child partition.\n");
            close(fd);
            return 1;
        }
        if (in_use < 0) {
            fprintf(stderr,
                    "Refusing to format because block-device usage could not be verified safely.\n");
            close(fd);
            return 1;
        }

        /* The sysfs/mount checks are advisory preflight. Re-open the exact
         * target with Linux block-device exclusivity before the first write so
         * a target that became busy during preflight still fails closed. */
        close(fd);
        fd = open(path, O_RDWR | O_CLOEXEC | O_EXCL);
        if (fd < 0) {
            perror("exclusive block-device open");
            fprintf(stderr,
                    "Refusing to format because the block device could not be held exclusively.\n");
            return 1;
        }

        struct stat locked;
        if (fstat(fd, &locked) != 0 || !S_ISBLK(locked.st_mode) ||
            locked.st_rdev != original.st_rdev) {
            fprintf(stderr,
                    "Refusing to format because the block target changed during preflight.\n");
            close(fd);
            return 1;
        }
        uint64_t locked_size = 0;
        int locked_is_block = 0;
        if (infs_get_size_bytes(fd, &locked_size, &locked_is_block) != 0 ||
            !locked_is_block || locked_size != size_bytes) {
            fprintf(stderr,
                    "Refusing to format because the block target geometry changed during preflight.\n");
            close(fd);
            return 1;
        }
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        perror("exclusive storage lock");
        fprintf(stderr,
                "Refusing to format storage that is open by another InfiltratorFS process.\n");
        close(fd);
        return 1;
    }
    if (size_bytes < INFS_MIN_SIZE_BYTES) {
        fprintf(stderr, "Target is too small; minimum is 16 MiB.\n");
        close(fd);
        return 1;
    }

    const uint64_t total_blocks = size_bytes / INFS_BLOCK_SIZE;
    if (!total_blocks || total_blocks > INFS_ALLOCATION_TREE_MAX_BLOCKS) {
        fprintf(stderr, "Target exceeds the Format 0.17 allocation-map capacity.\n");
        close(fd);
        return 1;
    }
    const uint64_t leaf_count =
        total_blocks / INFS_ALLOCATION_BITS_PER_LEAF +
        ((total_blocks % INFS_ALLOCATION_BITS_PER_LEAF) != 0);
    const uint64_t level1_count =
        leaf_count / INFS_ALLOCATION_TREE_FANOUT +
        ((leaf_count % INFS_ALLOCATION_TREE_FANOUT) != 0);
    const uint64_t level2_count =
        level1_count / INFS_ALLOCATION_TREE_FANOUT +
        ((level1_count % INFS_ALLOCATION_TREE_FANOUT) != 0);
    const uint64_t allocation_root = 1u;
    const uint64_t level2_start = allocation_root + 1u;
    const uint64_t level1_start = level2_start + level2_count;
    const uint64_t leaf_start = level1_start + level1_count;
    const uint64_t index_block = leaf_start + leaf_count;
    const uint64_t root_block = index_block + 1u;
    const uint64_t index_page_block = root_block + 1u;
    const uint64_t checkpoints[INFS_CHECKPOINT_COUNT] = {
        0,
        total_blocks / 2u,
        total_blocks - 1u
    };

    /*
     * UDisks/libblockdev performs a signature-clean step before mkfs and waits
     * for the block object to become filesystem-empty.  util-linux/libblkid
     * does not yet know the InfiltratorFS on-disk signature, so provide a
     * narrow helper mode that removes only a positively identified
     * InfiltratorFS checkpoint set.  Exit status 3 means "not InfiltratorFS";
     * callers may then continue with their normal generic signature cleaner.
     */
    if (wipe_only) {
        uint8_t raw[INFS_BLOCK_SIZE];
        struct infs_superblock_disk existing;
        uint64_t wipe_checkpoints[INFS_CHECKPOINT_COUNT] = {
            checkpoints[0], checkpoints[1], checkpoints[2]
        };
        int identified = 0;

        /*
         * Resize deliberately allows filesystem geometry to differ from the
         * backing partition.  Prefer the valid block-zero geometry anchor and
         * wipe the checkpoint locations recorded by the filesystem itself.
         * The geometric fallback retains recovery for historical, unresized
         * volumes whose block-zero copy is damaged.
         */
        if (infs_pread_full(fd, raw, sizeof(raw), 0) == 0 &&
            infs_decode_superblock(raw, &existing) == 0) {
            uint64_t fs_blocks = infs_le64_to_cpu(existing.total_blocks);
            uint64_t c0 =
                infs_le64_to_cpu(existing.checkpoint_block[0]);
            uint64_t c1 =
                infs_le64_to_cpu(existing.checkpoint_block[1]);
            uint64_t c2 =
                infs_le64_to_cpu(existing.checkpoint_block[2]);
            if (fs_blocks >= 3u && fs_blocks <= total_blocks &&
                c0 == 0 && c1 < fs_blocks && c2 < fs_blocks &&
                c1 != c0 && c2 != c0 && c2 != c1) {
                wipe_checkpoints[0] = c0;
                wipe_checkpoints[1] = c1;
                wipe_checkpoints[2] = c2;
                identified = 1;
            }
        }

        if (!identified) {
            for (unsigned i = 1; i < INFS_CHECKPOINT_COUNT; ++i) {
                off_t off =
                    (off_t)(checkpoints[i] * INFS_BLOCK_SIZE);
                if (infs_pread_full(fd, raw, sizeof(raw), off) != 0 ||
                    infs_decode_superblock(raw, &existing) != 0 ||
                    infs_le64_to_cpu(existing.total_blocks) != total_blocks)
                    continue;
                if (infs_le64_to_cpu(existing.checkpoint_block[0]) !=
                        checkpoints[0] ||
                    infs_le64_to_cpu(existing.checkpoint_block[1]) !=
                        checkpoints[1] ||
                    infs_le64_to_cpu(existing.checkpoint_block[2]) !=
                        checkpoints[2])
                    continue;
                identified = 1;
                break;
            }
        }

        if (!identified) {
            fprintf(stderr, "Target is not an InfiltratorFS volume.\n");
            close(fd);
            return 3;
        }

        memset(raw, 0, sizeof(raw));
        for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
            off_t off =
                (off_t)(wipe_checkpoints[i] * INFS_BLOCK_SIZE);
            if (infs_pwrite_full(fd, raw, sizeof(raw), off) != 0) {
                perror("wipe checkpoint");
                close(fd);
                return 1;
            }
        }
        if (fsync(fd) != 0) {
            perror("fsync signature wipe");
            close(fd);
            return 1;
        }

        close(fd);
        printf("Cleared InfiltratorFS signatures on %s.\n", path);
        return 0;
    }

    if (index_page_block >= checkpoints[1]) {
        fprintf(stderr, "Target is too small for the Format 0.17 metadata layout.\n");
        close(fd);
        return 1;
    }

    const uint64_t bitmap_bytes = (total_blocks + 7u) / 8u;
    const uint64_t bitmap_blocks =
        (bitmap_bytes + INFS_BLOCK_SIZE - 1u) / INFS_BLOCK_SIZE;
    if (!bitmap_blocks || bitmap_blocks > SIZE_MAX / INFS_BLOCK_SIZE) {
        fprintf(stderr, "Target allocation bitmap is too large for this build.\n");
        close(fd);
        return 1;
    }
    size_t bitmap_alloc = (size_t)(bitmap_blocks * INFS_BLOCK_SIZE);
    uint8_t *bitmap = calloc(1, bitmap_alloc);
    if (!bitmap) {
        perror("calloc");
        close(fd);
        return 1;
    }
