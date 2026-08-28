// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/format.h"
#include "infilfs/fs.h"
#include "infilfs/posix_io.h"
#include "infilfs/volume.h"

#include "infilfs/endian.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void print_udev_label(const uint8_t label[INFS_LABEL_MAX])
{
    char clean[INFS_LABEL_MAX + 1u];
    size_t used = 0;

    for (size_t i = 0; i < INFS_LABEL_MAX && label[i] != 0; ++i) {
        unsigned char c = label[i];
        if (c < 0x20u || c == 0x7fu || c == '=' || c == '\\')
            c = '_';
        clean[used++] = (char)c;
    }
    clean[used] = '\0';
    if (used)
        printf("ID_FS_LABEL=%s\n", clean);
}

int main(int argc, char **argv)
{
    int udev_mode = 0;
    const char *target = NULL;

    if (argc == 2) {
        target = argv[1];
    } else if (argc == 3 && strcmp(argv[1], "--udev") == 0) {
        udev_mode = 1;
        target = argv[2];
    } else {
        fprintf(stderr,
                "Usage: %s [--udev] <image-or-block-device>\n",
                argv[0]);
        return 2;
    }

    struct infs_storage storage = {0};
    infs_status status = infs_posix_storage_open(&storage, target, 0);
    if (status != INFS_STATUS_OK) {
        if (!udev_mode)
            fprintf(stderr, "open: %s\n", infs_status_string(status));
        return 1;
    }

    uint64_t size_bytes = 0;
    int is_block = 0;
    status = infs_storage_get_size(&storage, &size_bytes, &is_block);
    if (status != INFS_STATUS_OK) {
        if (!udev_mode)
            fprintf(stderr, "size: %s\n", infs_status_string(status));
        infs_storage_close(&storage);
        return 1;
    }
    (void)is_block;

    struct infs_superblock_disk sb;
    unsigned valid = 0;
    if (infs_read_best_superblock(&storage, size_bytes, &sb, &valid) != 0) {
        if (!udev_mode)
            fprintf(stderr, "No valid InfiltratorFS checkpoint found.\n");
        infs_storage_close(&storage);
        return 1;
    }

    char uuid_text[37];
    infs_uuid_to_string(sb.filesystem_uuid, uuid_text);

    if (udev_mode) {
        printf("ID_FS_USAGE=filesystem\n");
        printf("ID_FS_TYPE=infiltratorfs\n");
        printf("ID_FS_VERSION=%u.%u\n",
               infs_le16_to_cpu(sb.format_major),
               infs_le16_to_cpu(sb.format_minor));
        printf("ID_FS_UUID=%s\n", uuid_text);
        print_udev_label(sb.label);
        printf("ID_FS_BLOCK_SIZE=%u\n",
               1u << infs_le16_to_cpu(sb.block_shift));
        infs_storage_close(&storage);
        return 0;
    }

    uint8_t root[INFS_BLOCK_SIZE], index[INFS_BLOCK_SIZE];
    uint64_t root_no = infs_le64_to_cpu(sb.root_object_block);
    uint64_t index_no = infs_le64_to_cpu(sb.object_index_block);
    int root_ok = infs_storage_read(&storage, root_no * INFS_BLOCK_SIZE,
                                    root, sizeof(root)) == 0 &&
                  infs_validate_object_block(root);
    int index_ok = infs_storage_read(&storage, index_no * INFS_BLOCK_SIZE,
                                     index, sizeof(index)) == 0 &&
                   infs_validate_object_block(index);

    printf("InfiltratorFS\n");
    printf("  Format:          %u.%u\n",
           infs_le16_to_cpu(sb.format_major), infs_le16_to_cpu(sb.format_minor));
    printf("  UUID:            %s\n", uuid_text);
    printf("  Label:           %.*s\n", (int)(INFS_LABEL_MAX - 1u),
           (const char *)sb.label);
    printf("  Generation:      %" PRIu64 "\n", infs_le64_to_cpu(sb.generation));
    printf("  Block size:      %u bytes\n", 1u << infs_le16_to_cpu(sb.block_shift));
    printf("  Total blocks:    %" PRIu64 "\n", infs_le64_to_cpu(sb.total_blocks));
    printf("  Free blocks:     %" PRIu64 "\n", infs_le64_to_cpu(sb.free_blocks));
    printf("  Allocation root: block %" PRIu64 "\n",
           infs_le64_to_cpu(sb.allocation_root_block));
    printf("  Allocation leaves: %" PRIu64 "\n",
           infs_le64_to_cpu(sb.allocation_leaf_count));
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
        if (infs_storage_read(&storage, cp[i] * INFS_BLOCK_SIZE,
                              raw, sizeof(raw)) == 0 &&
            infs_decode_superblock(raw, &copy) == 0)
            printf(" %" PRIu64, infs_le64_to_cpu(copy.generation));
        else
            printf(" invalid");
    }
    printf("\n");

    infs_storage_close(&storage);
    return root_ok && index_ok ? 0 : 1;
}
