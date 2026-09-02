// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "infiltratorfs_ioctl.h"

struct optimize_options {
    bool defrag;
    bool recursive;
    unsigned int passes;
    uint64_t max_bytes;
};

struct optimize_summary {
    uint64_t files;
    uint64_t fragmented_files;
    uint64_t before_extents;
    uint64_t after_extents;
    uint64_t moved_bytes;
    uint64_t failures;
};

static struct optimize_options g_options;
static struct optimize_summary g_summary;

static void usage(const char *program)
{
    fprintf(stderr,
        "Usage: %s [--metrics|--defrag] [--recursive] "
        "[--max-mib N] [--passes N] PATH\n",
        program);
}

static int parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno || !end || *end != '\0')
        return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static int get_metrics(int fd, struct infilfs_fragmentation_metrics *metrics)
{
    memset(metrics, 0, sizeof(*metrics));
    return ioctl(fd, INFILFS_IOC_GET_FRAGMENTATION, metrics);
}

static void print_metrics(const char *path,
                          const struct infilfs_fragmentation_metrics *m)
{
    printf("%s: extents=%" PRIu64 " physical-runs=%" PRIu64
           " holes=%" PRIu64 " allocated=%" PRIu64
           " blocks largest=%" PRIu64
           " blocks fragmentation=%.1f%% generation=%" PRIu64 "\n",
           path, (uint64_t)m->data_extents, (uint64_t)m->physical_runs,
           (uint64_t)m->hole_extents, (uint64_t)m->allocated_blocks,
           (uint64_t)m->largest_data_extent_blocks,
           (double)m->fragmentation_milli / 10.0,
           (uint64_t)m->generation);
}

static int optimize_file(const char *path)
{
    struct infilfs_fragmentation_metrics before;
    struct infilfs_fragmentation_metrics after;
    int flags = g_options.defrag ? O_RDWR : O_RDONLY;
    int fd;
    unsigned int pass;
    uint64_t moved = 0;

    fd = open(path, flags | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "%s: open failed: %s\n", path, strerror(errno));
        g_summary.failures++;
        return -1;
    }

    if (get_metrics(fd, &before) != 0) {
        int saved = errno;
        close(fd);
        if (saved == ENOTTY)
            fprintf(stderr, "%s: not a native InfiltratorFS regular file\n", path);
        else
            fprintf(stderr, "%s: metrics failed: %s\n", path, strerror(saved));
        g_summary.failures++;
        errno = saved;
        return -1;
    }

    g_summary.files++;
    g_summary.before_extents += before.data_extents;
    if (before.data_extents > 1)
        g_summary.fragmented_files++;

    if (!g_options.defrag) {
        print_metrics(path, &before);
        g_summary.after_extents += before.data_extents;
        close(fd);
        return 0;
    }

    after = before;
    for (pass = 0; pass < g_options.passes; ++pass) {
        struct infilfs_defrag_request request;

        memset(&request, 0, sizeof(request));
        request.max_bytes = g_options.max_bytes;
        if (ioctl(fd, INFILFS_IOC_DEFRAG_FILE, &request) != 0) {
            int saved = errno;
            fprintf(stderr, "%s: defrag pass %u failed: %s\n",
                    path, pass + 1u, strerror(saved));
            close(fd);
            g_summary.failures++;
            errno = saved;
            return -1;
        }
        moved += request.moved_bytes;
        if (!request.moved_bytes)
            break;
    }

    if (fsync(fd) != 0) {
        int saved = errno;
        fprintf(stderr, "%s: fsync failed: %s\n", path, strerror(saved));
        close(fd);
        g_summary.failures++;
        errno = saved;
        return -1;
    }

    if (get_metrics(fd, &after) != 0) {
        int saved = errno;
        fprintf(stderr, "%s: post-defrag metrics failed: %s\n",
                path, strerror(saved));
        close(fd);
        g_summary.failures++;
        errno = saved;
        return -1;
    }

    printf("%s: extents %" PRIu64 " -> %" PRIu64
           ", fragmentation %.1f%% -> %.1f%%, moved %.2f MiB\n",
           path, (uint64_t)before.data_extents,
           (uint64_t)after.data_extents,
           (double)before.fragmentation_milli / 10.0,
           (double)after.fragmentation_milli / 10.0,
           (double)moved / (1024.0 * 1024.0));

    g_summary.after_extents += after.data_extents;
    g_summary.moved_bytes += moved;
    close(fd);
    return 0;
}

static int walk_callback(const char *path, const struct stat *st,
                         int typeflag, struct FTW *ftwbuf)
{
    (void)ftwbuf;
    if (typeflag == FTW_F && S_ISREG(st->st_mode))
        (void)optimize_file(path);
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    struct stat st;
    int i;

    memset(&g_options, 0, sizeof(g_options));
    g_options.passes = 1024u;
    g_options.max_bytes = 64ULL * 1024ULL * 1024ULL;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--metrics") == 0) {
            g_options.defrag = false;
        } else if (strcmp(argv[i], "--defrag") == 0) {
            g_options.defrag = true;
        } else if (strcmp(argv[i], "--recursive") == 0) {
            g_options.recursive = true;
        } else if (strcmp(argv[i], "--max-mib") == 0 && i + 1 < argc) {
            uint64_t mib;
            if (parse_u64(argv[++i], &mib) != 0 ||
                !mib || mib > 512u ||
                mib > UINT64_MAX / (1024u * 1024u)) {
                usage(argv[0]);
                return 2;
            }
            g_options.max_bytes = mib * 1024u * 1024u;
        } else if (strcmp(argv[i], "--passes") == 0 && i + 1 < argc) {
            uint64_t passes;
            if (parse_u64(argv[++i], &passes) != 0 ||
                !passes || passes > 100000u) {
                usage(argv[0]);
                return 2;
            }
            g_options.passes = (unsigned int)passes;
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else if (!path) {
            path = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!path) {
        usage(argv[0]);
        return 2;
    }
    if (lstat(path, &st) != 0) {
        perror(path);
        return 1;
    }

    if (S_ISREG(st.st_mode)) {
        (void)optimize_file(path);
    } else if (S_ISDIR(st.st_mode) && g_options.recursive) {
        if (nftw(path, walk_callback, 32, FTW_PHYS | FTW_MOUNT) != 0) {
            perror("nftw");
            return 1;
        }
    } else {
        fprintf(stderr, "%s: use --recursive for a directory\n", path);
        return 2;
    }

    printf("Summary: files=%" PRIu64 " fragmented=%" PRIu64
           " extents=%" PRIu64 "->%" PRIu64
           " moved=%.2f MiB failures=%" PRIu64 "\n",
           g_summary.files, g_summary.fragmented_files,
           g_summary.before_extents, g_summary.after_extents,
           (double)g_summary.moved_bytes / (1024.0 * 1024.0),
           g_summary.failures);

    return g_summary.failures ? 1 : 0;
}
