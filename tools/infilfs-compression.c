// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/posix_io.h"
#include "infilfs/volume.h"
#include "infiltratr/core.h"
#include "infiltratr/format.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

static int block_device_is_mounted(const char *path)
{
    struct stat st;
    FILE *stream;
    char *line = NULL;
    size_t capacity = 0;
    int mounted = 0;

    if (stat(path, &st) != 0 || !S_ISBLK(st.st_mode))
        return 0;
    stream = fopen("/proc/self/mountinfo", "r");
    if (!stream)
        return -1;
    while (getline(&line, &capacity, stream) >= 0) {
        unsigned int major_no = 0;
        unsigned int minor_no = 0;
        if (sscanf(line, "%*u %*u %u:%u", &major_no, &minor_no) == 2 &&
            major_no == major(st.st_rdev) &&
            minor_no == minor(st.st_rdev)) {
            mounted = 1;
            break;
        }
    }
    free(line);
    fclose(stream);
    return mounted;
}

static void format_gib(uint64_t bytes, char *buffer, size_t size)
{
    static const char *const units[] = {"B", "KiB", "MiB", "GiB"};
    InfiltratrScaleOptions options = INFILTRATR_SCALE_OPTIONS_INIT;

    options.minimum_unit = 3u;
    options.maximum_unit = 3u;
    options.decimal_places = 2u;
    options.integer_threshold = 0.0L;
    options.integer_at_minimum_unit = false;
    (void)infiltratr_format_scaled_quantity(
        (long double)bytes, units, INFILTRATR_ARRAY_LENGTH(units),
        "", &options, buffer, size);
}

static void print_bytes(const char *label, uint64_t value)
{
    char display[64];
    format_gib(value, display, sizeof(display));
    printf("  %-34s %" PRIu64 " bytes (%s)\n", label, value, display);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr,
            "Usage: %s <unmounted-infiltratorfs-image-or-device>\n",
            argv[0]);
        return 2;
    }

    int mounted = block_device_is_mounted(argv[1]);
    if (mounted < 0) {
        fprintf(stderr, "infilfs-compression: cannot inspect mount table\n");
        return 1;
    }
    if (mounted) {
        fprintf(stderr,
            "infilfs-compression: refusing direct-device scan of a mounted "
            "filesystem; unmount it first\n");
        return 1;
    }

    struct infs_volume vol;
    infs_status status = infs_posix_volume_open(&vol, argv[1], 0);
    if (status != INFS_STATUS_OK) {
        fprintf(stderr, "infilfs-compression: open: %s\n",
                infs_status_string(status));
        return 1;
    }

    struct infs_compression_metrics metrics;
    status = infs_compression_metrics(&vol, &metrics);
    infs_volume_close(&vol);
    if (status != INFS_STATUS_OK) {
        fprintf(stderr, "infilfs-compression: metrics: %s\n",
                infs_status_string(status));
        return 1;
    }

    printf("InfiltratorFS compression metrics\n");
    printf("  Generation:                        %" PRIu64 "\n",
           metrics.generation);
    printf("  Files scanned:                     %" PRIu64 "\n",
           metrics.files_scanned);
    printf("  Retained snapshots scanned:        %" PRIu64 "\n",
           metrics.snapshots_scanned);
    print_bytes("Referenced logical file bytes:",
                metrics.referenced_logical_bytes);
    print_bytes("Compressed referenced logical:",
                metrics.compressed_referenced_logical_bytes);
    print_bytes("Unique compressed logical:",
                metrics.unique_compressed_logical_bytes);
    print_bytes("Unique compressed physical:",
                metrics.unique_compressed_physical_bytes);
    print_bytes("Physical space saved by compression:",
                metrics.compression_saved_bytes);
    printf("  Unique compressed streams:         %" PRIu64 "\n",
           metrics.unique_compressed_streams);
    double pct = infiltratr_percent_u64(
        metrics.compression_saved_bytes,
        metrics.unique_compressed_logical_bytes);
    printf("  Compression saving on compressed data: %.2f%%\n", pct);
    puts("  Note: savings exclude sparse holes and reflink/snapshot sharing.");
    return 0;
}
