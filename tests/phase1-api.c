// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/volume.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void fail(const char *what)
{
    fprintf(stderr, "phase1-api: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void expect(int condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "phase1-api: assertion failed: %s\n", what);
        exit(1);
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <formatted-image>\n", argv[0]);
        return 2;
    }

    struct infs_volume vol;
    if (infs_volume_open(&vol, argv[1], 1) != 0)
        fail("open");

    if (infs_mkdir(&vol, "/alpha", 0750, getuid(), getgid()) != 0)
        fail("mkdir alpha");
    if (infs_mkdir(&vol, "/beta", 0755, getuid(), getgid()) != 0)
        fail("mkdir beta");
    if (infs_create_file(&vol, "/alpha/data", 0640, getuid(), getgid()) != 0)
        fail("create data");

    const char first[] = "ABCDEFGHIJ";
    if (infs_write_file(&vol, "/alpha/data", first, sizeof(first) - 1u, 0) !=
        (ssize_t)(sizeof(first) - 1u))
        fail("initial write");

    const char tail[] = "XYZ";
    off_t tail_off = (off_t)(INFS_BLOCK_SIZE * 2u + 3u);
    if (infs_write_file(&vol, "/alpha/data", tail, sizeof(tail) - 1u, tail_off) !=
        (ssize_t)(sizeof(tail) - 1u))
        fail("sparse-style write");

    unsigned char probe[32];
    memset(probe, 0xff, sizeof(probe));
    ssize_t n = infs_read_file(&vol, "/alpha/data", probe, sizeof(probe),
                               (off_t)INFS_BLOCK_SIZE + 100);
    expect(n == (ssize_t)sizeof(probe), "hole probe length");
    for (size_t i = 0; i < sizeof(probe); ++i)
        expect(probe[i] == 0, "unwritten gap must read as zero");

    if (infs_truncate_file(&vol, "/alpha/data", 5) != 0)
        fail("truncate shrink");
    if (infs_truncate_file(&vol, "/alpha/data", 10) != 0)
        fail("truncate regrow");
    unsigned char ten[10];
    memset(ten, 0xff, sizeof(ten));
    n = infs_read_file(&vol, "/alpha/data", ten, sizeof(ten), 0);
    expect(n == 10, "regrown read length");
    expect(memcmp(ten, "ABCDE", 5) == 0, "prefix survived truncate");
    for (size_t i = 5; i < 10; ++i)
        expect(ten[i] == 0, "regrown bytes must be zero");

    if (infs_chmod(&vol, "/alpha/data", 0600) != 0)
        fail("chmod");
    struct stat st;
    if (infs_getattr(&vol, "/alpha/data", &st) != 0)
        fail("getattr");
    expect((st.st_mode & 07777) == 0600, "chmod persisted");
    expect(st.st_size == 10, "size after regrow");

    if (infs_rename(&vol, "/alpha/data", "/beta/moved") != 0)
        fail("cross-directory rename");
    errno = 0;
    expect(infs_getattr(&vol, "/alpha/data", &st) != 0 && errno == ENOENT,
           "old name removed");
    if (infs_getattr(&vol, "/beta/moved", &st) != 0)
        fail("new name lookup");

    if (infs_unlink(&vol, "/beta/moved") != 0)
        fail("unlink");
    if (infs_rmdir(&vol, "/alpha") != 0)
        fail("rmdir alpha");
    if (infs_rmdir(&vol, "/beta") != 0)
        fail("rmdir beta");

    infs_volume_close(&vol);

    if (infs_volume_open(&vol, argv[1], 0) != 0)
        fail("reopen read-only");
    struct infs_dir_item *items = NULL;
    size_t count = 0;
    if (infs_list_dir(&vol, "/", &items, &count) != 0)
        fail("list root after reopen");
    expect(count == 0, "root empty after cleanup");
    infs_free_dir_items(items);
    infs_volume_close(&vol);

    puts("phase1-api: PASS");
    return 0;
}
