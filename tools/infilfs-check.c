// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/check.h"
#include "infilfs/posix_io.h"
#include "infilfs/volume.h"

#include <inttypes.h>
#include <stdio.h>

static const char *stage_name(uint32_t stage)
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

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <image-or-device>\n", argv[0]);
        return 1;
    }

    struct infs_volume vol;
    infs_status status = infs_posix_volume_open(&vol, argv[1], 0);
    if (status != INFS_STATUS_OK) {
        fprintf(stderr, "infilfs-check: open: %s\n",
                infs_status_string(status));
        return status == INFS_STATUS_CORRUPT ? 2 : 1;
    }

    struct infs_check_report report;
    status = infs_check(&vol, &report);
    infs_volume_close(&vol);

    printf("InfiltratorFS filesystem check\n");
    printf("  Generation:           %" PRIu64 "\n", report.check_generation);
    printf("  Checkpoint replicas:  %u/%u %s\n",
           report.checkpoint_replicas_valid, INFS_CHECKPOINT_COUNT,
           report.checkpoint_replicas_valid == INFS_CHECKPOINT_COUNT ?
               "OK" : "DEGRADED");
    printf("  Object index:         %s\n",
           check_word(report.object_index_valid, report.failed_stage,
                      INFS_CHECK_STAGE_OBJECT_INDEX));
    printf("  Allocation/ownership: %s\n",
           check_word(report.allocation_ownership_valid, report.failed_stage,
                      INFS_CHECK_STAGE_ALLOCATION_OWNERSHIP));
    printf("  Namespace/references: %s\n",
           check_word(report.namespace_valid, report.failed_stage,
                      INFS_CHECK_STAGE_NAMESPACE));
    printf("  Checksum metadata:    %s\n",
           check_word(report.checksum_metadata_valid, report.failed_stage,
                      INFS_CHECK_STAGE_CHECKSUM_METADATA));
    puts("  User-data checksum scan: NOT REQUESTED (--scrub for deep verification)");

    if (status == INFS_STATUS_OK) {
        puts("  Result:               CLEAN");
        return 0;
    }

    if (status == INFS_STATUS_CORRUPT || status == INFS_STATUS_NOT_FOUND ||
        status == INFS_STATUS_LOOP_DETECTED) {
        printf("  Failed stage:         %s\n", stage_name(report.failed_stage));
        puts("  Result:               FILESYSTEM ERRORS DETECTED");
        return 2;
    }

    fprintf(stderr, "infilfs-check: check: %s\n", infs_status_string(status));
    return 1;
}
