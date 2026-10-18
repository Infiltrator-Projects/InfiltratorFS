// SPDX-License-Identifier: GPL-3.0-or-later
/* Exact-source Format 0.19 regular-file write/persistence qualification.
 * Never opens block devices for experimental writes. */
#include "infilfs/posix_io.h"
#include "infilfs/volume.h"
#include "infilfs/endian.h"
#include "infilfs/fs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA_BYTES (256u * 1024u)

static void require(int condition, const char *stage)
{
    if (!condition) {
        fprintf(stderr, "format-019-posix: %s\n", stage);
        exit(1);
    }
}

static infs_status open_image(struct infs_volume *vol,
                               const char *path, int writable)
{
    struct infs_storage storage = {0};
    infs_status status = infs_posix_storage_open(
        &storage, path, writable);
    if (status != INFS_STATUS_OK)
        return status;
    status = writable ?
        infs_volume_open_019_rw_test_image(vol, &storage) :
        infs_volume_open_019_test_image(vol, &storage);
    if (status != INFS_STATUS_OK)
        infs_storage_close(&storage);
    return status;
}

int main(int argc, char **argv)
{
    struct infs_volume vol = {0};
    struct infs_scrub_report report = {0};
    struct infs_create_options opts = {
        .posix_permissions = 0644u,
        .posix_uid = 1000u,
        .posix_gid = 1000u,
    };
    uint8_t *payload;
    uint8_t *readback;
    uint64_t generation;
    if (argc != 2) {
        fputs("Usage: format-019-posix <disposable-regular-image>\n", stderr);
        return 2;
    }
    payload = malloc(DATA_BYTES);
    readback = malloc(DATA_BYTES);
    require(payload && readback, "allocate test payload");
    for (size_t i = 0; i < DATA_BYTES; ++i)
        payload[i] = (uint8_t)((i * 109u + (i >> 9)) & 255u);

    require(open_image(&vol, argv[1], 1) == INFS_STATUS_OK,
            "writable 0.19 POSIX image open");
    require(infs_create_file(&vol, "/persistent", &opts) == INFS_STATUS_OK,
            "0.19 POSIX create");
    require(infs_write_file(&vol, "/persistent", payload, DATA_BYTES, 0) ==
            DATA_BYTES, "0.19 POSIX 256-KiB write");
    require(infs_volume_sync(&vol) == INFS_STATUS_OK,
            "0.19 POSIX sync via real fsync");
    generation = infs_le64_to_cpu(vol.sb.generation);
    require(generation >= 3u, "commit generations persisted");
    infs_volume_close(&vol);

    require(open_image(&vol, argv[1], 0) == INFS_STATUS_OK,
            "0.19 POSIX read-only reopen");
    require(infs_le64_to_cpu(vol.sb.generation) == generation,
            "read-only reopen selects latest complete commit");
    require(infs_read_file(&vol, "/persistent", readback,
                           DATA_BYTES, 0) == DATA_BYTES &&
            memcmp(readback, payload, DATA_BYTES) == 0,
            "0.19 POSIX payload survives close and reopen");
    require(infs_scrub(&vol, &report) == INFS_STATUS_OK &&
            report.checksum_errors == 0 && report.metadata_errors == 0 &&
            report.data_blocks_checked > 0,
            "0.19 POSIX committed file deep scrub clean");
    infs_volume_close(&vol);

    require(open_image(&vol, argv[1], 1) == INFS_STATUS_OK,
            "healthy committed 0.19 POSIX image reopens writable");
    require(infs_volume_sync(&vol) == INFS_STATUS_OK,
            "0.19 POSIX idle flush");
    infs_volume_close(&vol);
    free(readback);
    free(payload);
    puts("Format 0.19 POSIX image write/reopen/scrub: PASS");
    return 0;
}
