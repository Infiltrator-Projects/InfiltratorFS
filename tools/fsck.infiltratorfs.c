// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/check.h"
#include "infilfs/posix_io.h"
#include "infilfs/volume.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FSCK_EXIT_CLEAN 0
#define FSCK_EXIT_UNCORRECTED 4
#define FSCK_EXIT_OPERATIONAL 8
#define FSCK_EXIT_USAGE 16

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [-afnp] <image-or-device>\n"
            "       %s --scrub [-afnp] <image-or-device>\n"
            "       %s --scrub --online [-afnp] <image-or-device>\n"
            "       %s --scrub --snapshot <name> [-afnp] <image-or-device>\n",
            program, program, program, program);
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
        fprintf(stderr, "fsck.infiltratorfs: open: %s\n",
                infs_status_string(status));
        return status == INFS_STATUS_CORRUPT ?
            FSCK_EXIT_UNCORRECTED : FSCK_EXIT_OPERATIONAL;
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
        return FSCK_EXIT_CLEAN;
    }

    if (status == INFS_STATUS_CORRUPT || status == INFS_STATUS_NOT_FOUND ||
        status == INFS_STATUS_LOOP_DETECTED) {
        printf("  Failed stage:          %s\n",
               check_stage_name(report.failed_stage));
        puts("  Result:                FILESYSTEM ERRORS DETECTED");
        return FSCK_EXIT_UNCORRECTED;
    }

    fprintf(stderr, "fsck.infiltratorfs: %s\n", infs_status_string(status));
    return FSCK_EXIT_OPERATIONAL;
}

static const char *scrub_phase_name(uint32_t phase)
{
    switch (phase) {
    case INFS_SCRUB_PHASE_METADATA:
        return "metadata";
    case INFS_SCRUB_PHASE_DATA:
        return "data";
    case INFS_SCRUB_PHASE_SNAPSHOTS:
        return "snapshots";
    case INFS_SCRUB_PHASE_COMPLETE:
        return "complete";
    default:
        return "working";
    }
}

static const char *scrub_metadata_stage_name(uint32_t stage)
{
    switch (stage) {
    case INFS_SCRUB_METADATA_INDEX_OBJECTS:
        return "index-objects";
    case INFS_SCRUB_METADATA_OWNERSHIP_OBJECTS:
        return "ownership-objects";
    case INFS_SCRUB_METADATA_SNAPSHOT_BITMAP:
        return "snapshot-bitmap";
    case INFS_SCRUB_METADATA_SNAPSHOT_UNION:
        return "snapshot-union";
    case INFS_SCRUB_METADATA_OWNERSHIP_BITMAP:
        return "ownership-bitmap";
    case INFS_SCRUB_METADATA_INTEGRITY_OBJECTS:
        return "integrity-objects";
    case INFS_SCRUB_METADATA_NAMESPACE_DIRECTORIES:
        return "namespace-directories";
    case INFS_SCRUB_METADATA_NAMESPACE_LINKS:
        return "namespace-links";
    case INFS_SCRUB_METADATA_NAMESPACE_REACHABILITY:
        return "namespace-reachability";
    case INFS_SCRUB_METADATA_CHECKSUM_FILES:
        return "checksum-files";
    case INFS_SCRUB_METADATA_SNAPSHOT_GENERATIONS:
        return "snapshot-generations";
    case INFS_SCRUB_METADATA_OWNERSHIP_DATA_BLOCKS:
        return "ownership-data-blocks";
    default:
        return "starting";
    }
}

static void scrub_progress(const struct infs_scrub_progress *progress,
                           void *context)
{
    (void)context;
    if (!progress)
        return;

    uint64_t errors = progress->checksum_errors + progress->metadata_errors;
    if (progress->phase == INFS_SCRUB_PHASE_METADATA) {
        double percent = progress->metadata_items_total ?
            (100.0 * (double)progress->metadata_items_checked /
             (double)progress->metadata_items_total) : 0.0;
        fprintf(stderr,
                "\rInfiltratorFS scrub: metadata/%-24s gen=%" PRIu64
                "  %" PRIu64 "/%" PRIu64 "  %6.2f%%  errors=%" PRIu64,
                scrub_metadata_stage_name(progress->metadata_stage),
                progress->metadata_generation,
                progress->metadata_items_checked,
                progress->metadata_items_total, percent, errors);
    } else {
        fprintf(stderr,
                "\rInfiltratorFS scrub: %-9s  files=%" PRIu64
                "  blocks=%" PRIu64 "  snapshots=%" PRIu64
                "  errors=%" PRIu64,
                scrub_phase_name(progress->phase), progress->files_checked,
                progress->data_blocks_checked, progress->snapshots_checked,
                errors);
    }
    if (progress->phase == INFS_SCRUB_PHASE_COMPLETE)
        fputc('\n', stderr);
    fflush(stderr);
}

static int run_deep_scrub(const char *target, int online,
                          const char *snapshot_name)
{
    struct infs_volume vol;
    infs_status status = infs_posix_volume_open(&vol, target, online ? 1 : 0);
    if (status != INFS_STATUS_OK) {
        fprintf(stderr, "fsck.infiltratorfs --scrub: open: %s\n",
                infs_status_string(status));
        return status == INFS_STATUS_CORRUPT ?
            FSCK_EXIT_UNCORRECTED : FSCK_EXIT_OPERATIONAL;
    }

    struct infs_scrub_report report;
    if (online) {
        fprintf(stderr, "InfiltratorFS scrub: preparing stable online view...\n");
        status = infs_scrub_online(&vol, &report);
    } else if (snapshot_name) {
        fprintf(stderr, "InfiltratorFS scrub: checking snapshot %s...\n",
                snapshot_name);
        status = infs_snapshot_scrub(&vol, snapshot_name, &report);
    } else {
        status = infs_scrub_with_progress(&vol, &report, scrub_progress, NULL);
    }

    if (status != INFS_STATUS_OK) {
        fprintf(stderr, "\nfsck.infiltratorfs --scrub: %s\n",
                infs_status_string(status));
        infs_volume_close(&vol);
        return status == INFS_STATUS_CORRUPT ?
            FSCK_EXIT_UNCORRECTED : FSCK_EXIT_OPERATIONAL;
    }
    infs_volume_close(&vol);

    printf("InfiltratorFS scrub\n");
    printf("  Generation:          %" PRIu64 "\n", report.scrub_generation);
    printf("  Files checked:       %" PRIu64 "\n", report.files_checked);
    printf("  Data blocks checked: %" PRIu64 "\n", report.data_blocks_checked);
    printf("  Snapshots checked:   %" PRIu64 "\n", report.snapshots_checked);
    printf("  Checksum errors:     %" PRIu64 "\n", report.checksum_errors);
    printf("  Metadata errors:     %" PRIu64 "\n", report.metadata_errors);

    if (report.checksum_errors || report.metadata_errors) {
        puts("  Result:              CORRUPTION DETECTED");
        return FSCK_EXIT_UNCORRECTED;
    }
    puts("  Result:              CLEAN");
    return FSCK_EXIT_CLEAN;
}

int main(int argc, char **argv)
{
    int deep_scrub = 0;
    int online = 0;
    const char *snapshot_name = NULL;
    const char *target = NULL;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "--scrub") == 0) {
            deep_scrub = 1;
        } else if (strcmp(arg, "--online") == 0) {
            online = 1;
        } else if (strcmp(arg, "--snapshot") == 0) {
            if (++i >= argc) {
                usage(argv[0]);
                return FSCK_EXIT_USAGE;
            }
            snapshot_name = argv[i];
        } else if (arg[0] == '-' && arg[1] != '\0') {
            if (arg[1] == '-') {
                fprintf(stderr, "fsck.infiltratorfs: unsupported option: %s\n", arg);
                usage(argv[0]);
                return FSCK_EXIT_USAGE;
            }
            for (const char *p = arg + 1; *p; ++p) {
                if (*p != 'a' && *p != 'f' && *p != 'n' && *p != 'p') {
                    fprintf(stderr,
                            "fsck.infiltratorfs: unsupported option: -%c\n", *p);
                    usage(argv[0]);
                    return FSCK_EXIT_USAGE;
                }
            }
        } else if (!target) {
            target = arg;
        } else {
            usage(argv[0]);
            return FSCK_EXIT_USAGE;
        }
    }

    if (!target) {
        usage(argv[0]);
        return FSCK_EXIT_USAGE;
    }
    if ((online || snapshot_name) && !deep_scrub) {
        fprintf(stderr,
                "fsck.infiltratorfs: --online and --snapshot require --scrub\n");
        return FSCK_EXIT_USAGE;
    }
    if (online && snapshot_name) {
        fprintf(stderr,
                "fsck.infiltratorfs: --online and --snapshot are mutually exclusive\n");
        return FSCK_EXIT_USAGE;
    }

    if (deep_scrub)
        return run_deep_scrub(target, online, snapshot_name);
    return run_structural_check(target);
}
