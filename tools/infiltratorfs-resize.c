// SPDX-License-Identifier: GPL-3.0-or-later
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "infiltratorfs_ioctl.h"

static int parse_size(const char *text, uint64_t *bytes)
{
    char *end = NULL;
    unsigned long long value;
    uint64_t multiplier = 1;

    if (!text || !*text)
        return -1;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno || end == text)
        return -1;

    if (*end) {
        if (strcmp(end, "KiB") == 0 || strcmp(end, "K") == 0)
            multiplier = UINT64_C(1024);
        else if (strcmp(end, "MiB") == 0 || strcmp(end, "M") == 0)
            multiplier = UINT64_C(1024) * 1024;
        else if (strcmp(end, "GiB") == 0 || strcmp(end, "G") == 0)
            multiplier = UINT64_C(1024) * 1024 * 1024;
        else if (strcmp(end, "TiB") == 0 || strcmp(end, "T") == 0)
            multiplier = UINT64_C(1024) * 1024 * 1024 * 1024;
        else
            return -1;
    }

    if ((uint64_t)value > UINT64_MAX / multiplier)
        return -1;
    *bytes = (uint64_t)value * multiplier;
    return 0;
}

int main(int argc, char **argv)
{
    struct infilfs_resize_request request;
    int fd;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s MOUNTPOINT SIZE|MAX\n", argv[0]);
        return 2;
    }

    memset(&request, 0, sizeof(request));
    if (strcasecmp(argv[2], "max") == 0) {
        request.flags = INFILFS_RESIZE_TO_DEVICE_MAX;
    } else if (parse_size(argv[2], &request.size_bytes) != 0) {
        fprintf(stderr,
                "Invalid size: %s (use bytes, KiB, MiB, GiB, TiB or max)\n",
                argv[2]);
        return 2;
    }

    fd = open(argv[1], O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (fd < 0) {
        perror(argv[1]);
        return 1;
    }
    if (ioctl(fd, INFILFS_IOC_RESIZE_VOLUME, &request) != 0) {
        perror("InfiltratorFS resize");
        close(fd);
        return 1;
    }
    close(fd);

    printf("old_size=%" PRIu64 "\nnew_size=%" PRIu64 "\n",
           (uint64_t)request.old_size_bytes,
           (uint64_t)request.new_size_bytes);
    return 0;
}
