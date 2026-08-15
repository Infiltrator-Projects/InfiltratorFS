// SPDX-License-Identifier: GPL-3.0-or-later
int main(int argc, char **argv)
{
    int force = 0;
    const char *label = "InfiltratorFS";
    const char *path = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--force") == 0) {
            force = 1;
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
        fprintf(stderr, "Label is too long (maximum %u bytes).\n", INFS_LABEL_MAX - 1u);
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
    if (size_bytes < INFS_MIN_SIZE_BYTES) {
        fprintf(stderr, "Target is too small; minimum is 16 MiB.\n");
        close(fd);
        return 1;
    }

    const uint64_t total_blocks = size_bytes / INFS_BLOCK_SIZE;
    const uint64_t bitmap_bytes = (total_blocks + 7u) / 8u;
    const uint64_t bitmap_blocks = (bitmap_bytes + INFS_BLOCK_SIZE - 1u) / INFS_BLOCK_SIZE;
    const uint64_t bitmap_start = 1u;
    const uint64_t index_block = bitmap_start + bitmap_blocks;
    const uint64_t root_block = index_block + 1u;
    const uint64_t checkpoints[INFS_CHECKPOINT_COUNT] = {
        0,
        total_blocks / 2u,
        total_blocks - 1u
    };

    if (root_block >= checkpoints[1]) {
        fprintf(stderr, "Target is too small for the format 0.6 metadata layout.\n");
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
