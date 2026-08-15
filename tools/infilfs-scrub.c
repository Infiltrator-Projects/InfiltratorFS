// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/volume.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <image-or-device>\n", argv[0]);
        return 2;
    }

    struct infs_volume vol;
    if (infs_volume_open(&vol, argv[1], 0) != 0) {
        fprintf(stderr, "infilfs-scrub: open: %s\n", strerror(errno));
        return 1;
    }

    struct infs_scrub_report report;
    if (infs_scrub(&vol, &report) != 0) {
        fprintf(stderr, "infilfs-scrub: scrub: %s\n", strerror(errno));
        infs_volume_close(&vol);
        return 1;
    }
    infs_volume_close(&vol);

    printf("InfiltratorFS scrub\n");
    printf("  Files checked:       %" PRIu64 "\n", report.files_checked);
    printf("  Data blocks checked: %" PRIu64 "\n", report.data_blocks_checked);
    printf("  Checksum errors:     %" PRIu64 "\n", report.checksum_errors);
    printf("  Metadata errors:     %" PRIu64 "\n", report.metadata_errors);

    if (report.checksum_errors || report.metadata_errors) {
        puts("  Result:              CORRUPTION DETECTED");
        return 2;
    }
    puts("  Result:              CLEAN");
    return 0;
}
