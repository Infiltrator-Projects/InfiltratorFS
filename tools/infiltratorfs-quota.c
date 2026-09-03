// SPDX-License-Identifier: GPL-3.0-or-later
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "infiltratorfs_ioctl.h"

static int parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long v;
    errno = 0;
    v = strtoul(text, &end, 10);
    if (errno || !text[0] || !end || *end || v > UINT32_MAX)
        return -1;
    *value = (uint32_t)v;
    return 0;
}

static int parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long v;
    errno = 0;
    v = strtoull(text, &end, 10);
    if (errno || !text[0] || !end || *end)
        return -1;
    *value = (uint64_t)v;
    return 0;
}

static int parse_size(const char *text, uint64_t *bytes)
{
    char *end = NULL;
    unsigned long long value;
    uint64_t multiplier = 1;

    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno || !text[0] || end == text)
        return -1;
    if (*end) {
        if (!strcmp(end, "KiB") || !strcmp(end, "K"))
            multiplier = UINT64_C(1024);
        else if (!strcmp(end, "MiB") || !strcmp(end, "M"))
            multiplier = UINT64_C(1024) * 1024;
        else if (!strcmp(end, "GiB") || !strcmp(end, "G"))
            multiplier = UINT64_C(1024) * 1024 * 1024;
        else if (!strcmp(end, "TiB") || !strcmp(end, "T"))
            multiplier = UINT64_C(1024) * 1024 * 1024 * 1024;
        else
            return -1;
    }
    if ((uint64_t)value > UINT64_MAX / multiplier)
        return -1;
    *bytes = (uint64_t)value * multiplier;
    return 0;
}

static int quota_type(const char *text, uint32_t *type)
{
    if (!strcmp(text, "user")) *type = INFILFS_QUOTA_USER;
    else if (!strcmp(text, "group")) *type = INFILFS_QUOTA_GROUP;
    else if (!strcmp(text, "project")) *type = INFILFS_QUOTA_PROJECT;
    else return -1;
    return 0;
}

static int open_path(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        perror(path);
    return fd;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s set MOUNTPOINT user|group|project ID HARD_BYTES HARD_OBJECTS\n"
        "  %s get MOUNTPOINT user|group|project ID\n"
        "  %s clear MOUNTPOINT user|group|project ID\n"
        "  %s project-set DIRECTORY PROJECT_ID\n"
        "  %s project-clear DIRECTORY\n"
        "  %s project-get PATH\n"
        "HARD_BYTES accepts bytes or KiB/MiB/GiB/TiB; 0 means unlimited.\n"
        "HARD_OBJECTS 0 means unlimited. Setting both limits to 0 removes a rule.\n",
        argv0, argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
    int fd;

    if (argc >= 2 && (!strcmp(argv[1], "set") ||
                      !strcmp(argv[1], "get") ||
                      !strcmp(argv[1], "clear"))) {
        struct infilfs_quota_request request;
        unsigned long command;

        if ((!strcmp(argv[1], "set") && argc != 7) ||
            (!strcmp(argv[1], "get") && argc != 5) ||
            (!strcmp(argv[1], "clear") && argc != 5)) {
            usage(argv[0]);
            return 2;
        }
        memset(&request, 0, sizeof(request));
        if (quota_type(argv[3], &request.type) || parse_u32(argv[4], &request.id)) {
            fprintf(stderr, "Invalid quota type or ID.\n");
            return 2;
        }
        if (!strcmp(argv[1], "set")) {
            if (parse_size(argv[5], &request.hard_bytes) ||
                parse_u64(argv[6], &request.hard_objects)) {
                fprintf(stderr, "Invalid quota limit.\n");
                return 2;
            }
            command = INFILFS_IOC_SET_QUOTA;
        } else if (!strcmp(argv[1], "clear")) {
            command = INFILFS_IOC_SET_QUOTA;
        } else {
            command = INFILFS_IOC_GET_QUOTA;
        }
        fd = open_path(argv[2]);
        if (fd < 0) return 1;
        if (ioctl(fd, command, &request) != 0) {
            perror("InfiltratorFS quota");
            close(fd);
            return 1;
        }
        close(fd);
        printf("type=%u\nid=%u\nhard_bytes=%" PRIu64
               "\nhard_objects=%" PRIu64 "\nused_bytes=%" PRIu64
               "\nused_objects=%" PRIu64 "\n",
               request.type, request.id,
               (uint64_t)request.hard_bytes, (uint64_t)request.hard_objects,
               (uint64_t)request.used_bytes, (uint64_t)request.used_objects);
        return 0;
    }

    if (argc >= 2 && (!strcmp(argv[1], "project-set") ||
                      !strcmp(argv[1], "project-clear") ||
                      !strcmp(argv[1], "project-get"))) {
        struct infilfs_project_request request;
        unsigned long command;

        if ((!strcmp(argv[1], "project-set") && argc != 4) ||
            ((!strcmp(argv[1], "project-clear") || !strcmp(argv[1], "project-get")) && argc != 3)) {
            usage(argv[0]);
            return 2;
        }
        memset(&request, 0, sizeof(request));
        if (!strcmp(argv[1], "project-set")) {
            if (parse_u32(argv[3], &request.project_id) || !request.project_id) {
                fprintf(stderr, "Invalid project ID.\n");
                return 2;
            }
            command = INFILFS_IOC_SET_PROJECT;
        } else if (!strcmp(argv[1], "project-clear")) {
            command = INFILFS_IOC_SET_PROJECT;
        } else {
            command = INFILFS_IOC_GET_PROJECT;
        }
        fd = open_path(argv[2]);
        if (fd < 0) return 1;
        if (ioctl(fd, command, &request) != 0) {
            perror("InfiltratorFS project");
            close(fd);
            return 1;
        }
        close(fd);
        printf("project_id=%u\neffective_project_id=%u\n",
               request.project_id, request.effective_project_id);
        return 0;
    }

    usage(argv[0]);
    return 2;
}
