// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/format.h"
#include "infilfs/fs.h"
#include "infilfs/posix_io.h"
#include "infilfs/utf8.h"

#include "infilfs/endian.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define INFS_MIN_SIZE_BYTES (16ull * 1024ull * 1024ull)

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [--force] [-L label] <image-or-block-device>\n", prog);
}

static void bitmap_set(uint8_t *bitmap, uint64_t block)
{
    bitmap[block >> 3] |= (uint8_t)(1u << (block & 7u));
}

static int bitmap_get(const uint8_t *bitmap, uint64_t block)
{
    return (bitmap[block >> 3] >> (block & 7u)) & 1u;
}

static int id_is_zero_local(const uint8_t id[16])
{
    uint8_t combined = 0;
    for (unsigned i = 0; i < 16; ++i)
        combined |= id[i];
    return combined == 0;
}

static int generate_id(uint8_t id[16], const uint8_t avoid1[16],
                       const uint8_t avoid2[16])
{
    for (unsigned attempt = 0; attempt < 32u; ++attempt) {
        if (infs_random_bytes(id, 16) != 0)
            return -1;
        if (id_is_zero_local(id))
            continue;
        if (avoid1 && memcmp(id, avoid1, 16) == 0)
            continue;
        if (avoid2 && memcmp(id, avoid2, 16) == 0)
            continue;
        return 0;
    }
    errno = EIO;
    return -1;
}

static int same_or_descendant_path(const char *parent, const char *candidate)
{
    size_t length = strlen(parent);
    if (strncmp(parent, candidate, length) != 0)
        return 0;
    return candidate[length] == '\0' || candidate[length] == '/';
}

static int directory_has_entries(const char *path)
{
    DIR *directory = opendir(path);
    if (!directory)
        return -1;
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0) {
            result = 1;
            break;
        }
    }
    closedir(directory);
    return result;
}

/* Return 1 when the target or any child partition is mounted/held by another
 * block layer, 0 when it is demonstrably unused, and -1 when safety cannot be
 * established. Formatting a real block target fails closed on -1. */
static int block_device_in_use(int fd)
{
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISBLK(st.st_mode))
        return -1;

    char target_link[PATH_MAX];
    char target_real[PATH_MAX];
    snprintf(target_link, sizeof(target_link), "/sys/dev/block/%u:%u",
             major(st.st_rdev), minor(st.st_rdev));
    if (!realpath(target_link, target_real))
        return -1;

    FILE *mountinfo = fopen("/proc/self/mountinfo", "r");
    if (!mountinfo)
        return -1;
    char line[8192];
    while (fgets(line, sizeof(line), mountinfo)) {
        unsigned mount_major = 0, mount_minor = 0;
        if (sscanf(line, "%*u %*u %u:%u", &mount_major, &mount_minor) != 2)
            continue;
        char mount_link[PATH_MAX];
        char mount_real[PATH_MAX];
        snprintf(mount_link, sizeof(mount_link), "/sys/dev/block/%u:%u",
                 mount_major, mount_minor);
        if (realpath(mount_link, mount_real) &&
            same_or_descendant_path(target_real, mount_real)) {
            fclose(mountinfo);
            return 1;
        }
    }
    if (ferror(mountinfo)) {
        fclose(mountinfo);
        return -1;
    }
    fclose(mountinfo);

    DIR *classes = opendir("/sys/class/block");
    if (!classes)
        return -1;
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(classes)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        char class_path[PATH_MAX];
        char class_real[PATH_MAX];
        if (snprintf(class_path, sizeof(class_path), "/sys/class/block/%s",
                     entry->d_name) >= (int)sizeof(class_path) ||
            !realpath(class_path, class_real))
            continue;
        if (!same_or_descendant_path(target_real, class_real))
            continue;
        char holders[PATH_MAX];
        if (snprintf(holders, sizeof(holders), "%s/holders", class_path) >=
            (int)sizeof(holders)) {
            result = -1;
            break;
        }
        int held = directory_has_entries(holders);
        if (held < 0) {
            result = -1;
            break;
        }
        if (held > 0) {
            result = 1;
            break;
        }
    }
    closedir(classes);
    return result;
}
