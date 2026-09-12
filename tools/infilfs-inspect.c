// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/check.h"
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

static const char *check_stage_name(uint32_t stage)
{
    switch (stage) {
    case INFS_CHECK_STAGE_CHECKPOINTS:
        return "checkpoint replicas";
    case INFS_CHECK_STAGE_OBJECT_INDEX:
        return "object index";
    case INFS_CHECK_STAGE_ALLOCATION_OWNERSHIP:
        return "allocation/ownership metadata";
    case INFS_CHECK_STAGE_NAMESPACE:
        return "namespace/reference metadata";
    case INFS_CHECK_STAGE_CHECKSUM_METADATA:
        return "checksum metadata";
    case INFS_CHECK_STAGE_COMPLETE:
        return "complete";
    default:
        return "unknown";
    }
}

static const char *check_word(int valid, uint32_t failed_stage,
                              uint32_t this_stage)
{
    if (valid)
        return "OK";
    if (failed_stage == this_stage)
        return "FAILED";
    return "NOT CHECKED";
}

static int run_structural_check(const char *target)
{
    struct infs_volume vol;
    infs_status status = infs_posix_volume_open(&vol, target, 0);
    if (status != INFS_STATUS_OK) {
        fprintf(stderr, "infilfs-inspect --check: open: %s\n",
                infs_status_string(status));
        return status == INFS_STATUS_CORRUPT ? 2 : 1;
    }

    struct infs_check_report report;
    status = infs_check(&vol, &report);
    infs_volume_close(&vol);

    printf("InfiltratorFS filesystem check\n");
    printf("  Generation:            %" PRIu64 "\n", report.check_generation);
    printf("  Checkpoint replicas:   %u/%u %s\n",
           report.checkpoint_replicas_valid, INFS_CHECKPOINT_COUNT,
           report.checkpoint_replicas_valid == INFS_CHECKPOINT_COUNT ?
               "OK" : "DEGRADED");
    printf("  Object index:          %s\n",
           check_word(report.object_index_valid, report.failed_stage,
                      INFS_CHECK_STAGE_OBJECT_INDEX));
    printf("  Allocation/ownership:  %s\n",
           check_word(report.allocation_ownership_valid, report.failed_stage,
                      INFS_CHECK_STAGE_ALLOCATION_OWNERSHIP));
    printf("  Namespace/references:  %s\n",
           check_word(report.namespace_valid, report.failed_stage,
                      INFS_CHECK_STAGE_NAMESPACE));
    printf("  Checksum metadata:     %s\n",
           check_word(report.checksum_metadata_valid, report.failed_stage,
                      INFS_CHECK_STAGE_CHECKSUM_METADATA));
    puts("  User-data checksum scan: NOT REQUESTED (--scrub for deep verification)");

    if (status == INFS_STATUS_OK) {
        puts("  Result:                CLEAN");
        return 0;
    }

    if (status == INFS_STATUS_CORRUPT || status == INFS_STATUS_NOT_FOUND ||
        status == INFS_STATUS_LOOP_DETECTED) {
        printf("  Failed stage:          %s\n",
               check_stage_name(report.failed_stage));
        puts("  Result:                FILESYSTEM ERRORS DETECTED");
        return 2;
    }

    fprintf(stderr, "infilfs-inspect --check: %s\n",
            infs_status_string(status));
    return 1;
}

int main(int argc, char **argv)
{
    int udev_mode = 0;
    int check_mode = 0;
    const char *target = NULL;

    if (argc == 2) {
        target = argv[1];
    } else if (argc == 3 && strcmp(argv[1], "--udev") == 0) {
        udev_mode = 1;
        target = argv[2];
    } else if (argc == 3 && strcmp(argv[1], "--check") == 0) {
        check_mode = 1;
        target = argv[2];
    } else {
        fprintf(stderr,
                "Usage: %s [--udev|--check] <image-or-block-device>\n",
                argv[0]);
        return 2;
    }

    if (check_mode)
        return run_structural_check(target);

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
