// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/posix_io.h"
#include "infilfs/volume.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s <image-or-device>\n"
            "       %s --online <image-or-device>\n"
            "       %s --snapshot <name> <image-or-device>\n",
            program, program, program);
}

int main(int argc, char **argv)
{
    int online = argc == 3 && strcmp(argv[1], "--online") == 0;
    int snapshot = argc == 4 && strcmp(argv[1], "--snapshot") == 0;
    if (argc != 2 && !online && !snapshot) {
        usage(argv[0]);
        return 2;
    }

    const char *target = online ? argv[2] : snapshot ? argv[3] : argv[1];
    struct infs_volume vol;
    infs_status status = infs_posix_volume_open(&vol, target, online ? 1 : 0);
    if (status != INFS_STATUS_OK) {
        fprintf(stderr, "infilfs-scrub: open: %s\n",
                infs_status_string(status));
        return 1;
    }

    struct infs_scrub_report report;
    if (online)
        status = infs_scrub_online(&vol, &report);
    else if (snapshot)
        status = infs_snapshot_scrub(&vol, argv[2], &report);
    else
        status = infs_scrub(&vol, &report);
    if (status != INFS_STATUS_OK) {
        fprintf(stderr, "infilfs-scrub: scrub: %s\n",
                infs_status_string(status));
        infs_volume_close(&vol);
        return 1;
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
        return 2;
    }
    puts("  Result:              CLEAN");
    return 0;
}
