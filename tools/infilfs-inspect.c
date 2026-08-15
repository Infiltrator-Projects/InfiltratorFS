// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/format.h"
#include "infilfs/fs.h"
#include "infilfs/io.h"
#include "infilfs/volume.h"

#include <endian.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <image-or-block-device>\n", argv[0]);
        return 2;
    }

    int fd = open(argv[1], O_RDONLY | O_CLOEXEC);
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
    (void)is_block;

    struct infs_superblock_disk sb;
    unsigned valid = 0;
    if (infs_read_best_superblock(fd, size_bytes, &sb, &valid) != 0) {
        fprintf(stderr, "No valid InfiltratorFS checkpoint found.\n");
        close(fd);
        return 1;
    }

    uint8_t root[INFS_BLOCK_SIZE], index[INFS_BLOCK_SIZE];
    uint64_t root_no = le64toh(sb.root_object_block);
    uint64_t index_no = le64toh(sb.object_index_block);
    int root_ok = infs_pread_full(fd, root, sizeof(root),
                                  (off_t)(root_no * INFS_BLOCK_SIZE)) == 0 &&
                  infs_validate_object_block(root);
    int index_ok = infs_pread_full(fd, index, sizeof(index),
                                   (off_t)(index_no * INFS_BLOCK_SIZE)) == 0 &&
                   infs_validate_object_block(index);

    char uuid_text[37];
    infs_uuid_to_string(sb.filesystem_uuid, uuid_text);

    printf("InfiltratorFS\n");
    printf("  Format:          %u.%u\n",
           le16toh(sb.format_major), le16toh(sb.format_minor));
    printf("  UUID:            %s\n", uuid_text);
    printf("  Label:           %s\n", (char *)sb.label);
    printf("  Generation:      %" PRIu64 "\n", le64toh(sb.generation));
    printf("  Block size:      %u bytes\n", 1u << le16toh(sb.block_shift));
    printf("  Total blocks:    %" PRIu64 "\n", le64toh(sb.total_blocks));
    printf("  Free blocks:     %" PRIu64 "\n", le64toh(sb.free_blocks));
    printf("  Bitmap:          block %" PRIu64 ", %" PRIu64 " blocks\n",
           le64toh(sb.bitmap_start_block), le64toh(sb.bitmap_block_count));
    printf("  Object index:    block %" PRIu64 " (%s)\n",
           index_no, index_ok ? "valid" : "INVALID");
    printf("  Root object:     block %" PRIu64 " (%s)\n",
           root_no, root_ok ? "valid" : "INVALID");
    printf("  Valid checkpoints: %u/%u\n", valid, INFS_CHECKPOINT_COUNT);
    printf("  Checkpoint generations:");
    const uint64_t total_blocks = size_bytes / INFS_BLOCK_SIZE;
    const uint64_t cp[INFS_CHECKPOINT_COUNT] = {0, total_blocks / 2u, total_blocks - 1u};
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        uint8_t raw[INFS_BLOCK_SIZE];
        struct infs_superblock_disk copy;
        if (infs_pread_full(fd, raw, sizeof(raw), (off_t)(cp[i] * INFS_BLOCK_SIZE)) == 0 &&
            infs_decode_superblock(raw, &copy) == 0)
            printf(" %" PRIu64, le64toh(copy.generation));
        else
            printf(" invalid");
    }
    printf("\n");

    close(fd);
    return root_ok && index_ok ? 0 : 1;
}
