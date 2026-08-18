// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/posix_io.h"
#include "infilfs/volume.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fail(const char *what)
{
    fprintf(stderr, "phase1-api: %s\n", what);
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
    if (infs_posix_volume_open(&vol, argv[1], 1) != 0)
        fail("open");

    const struct infs_create_options directory_options = {
        .posix_permissions = 0750,
        .posix_uid = (uint32_t)getuid(),
        .posix_gid = (uint32_t)getgid(),
    };
    const struct infs_create_options file_options = {
        .posix_permissions = 0640,
        .posix_uid = (uint32_t)getuid(),
        .posix_gid = (uint32_t)getgid(),
    };

    if (infs_mkdir(&vol, "/alpha", &directory_options) != 0)
        fail("mkdir alpha");
    if (infs_mkdir(&vol, "/beta", &directory_options) != 0)
        fail("mkdir beta");
    if (infs_create_file(&vol, "/alpha/data", &file_options) != 0)
        fail("create data");
    struct infs_attributes initial_attributes;
    if (infs_get_attributes(&vol, "/alpha/data", &initial_attributes) != 0)
        fail("get attributes initial object");
    uint8_t stable_id[16];
    memcpy(stable_id, initial_attributes.object_id, sizeof(stable_id));

    struct infs_attributes path_attributes;
    expect(infs_get_attributes(&vol, "/alpha/data/", &path_attributes) ==
               INFS_STATUS_NOT_DIRECTORY,
           "trailing slash requires a directory");
    expect(infs_get_attributes(&vol, "/alpha/data/.", &path_attributes) ==
               INFS_STATUS_NOT_DIRECTORY,
           "dot cannot traverse a regular file");
    expect(infs_get_attributes(&vol, "/alpha/data/..", &path_attributes) ==
               INFS_STATUS_NOT_DIRECTORY,
           "dot-dot cannot traverse a regular file");
    expect(infs_get_attributes(&vol, "/alpha/./data", &path_attributes) ==
               INFS_STATUS_OK,
           "dot traversal through a directory works");
    expect(infs_get_attributes(&vol, "/alpha/../alpha/data", &path_attributes) ==
               INFS_STATUS_OK,
           "dot-dot traversal through a directory works");
    expect(infs_create_file(&vol, "/alpha/not-a-directory/", &file_options) ==
               INFS_STATUS_NOT_DIRECTORY,
           "create-file trailing slash requires directory semantics");
    expect(infs_unlink(&vol, "/alpha/data/") == INFS_STATUS_NOT_DIRECTORY,
           "unlink trailing slash requires directory semantics");

    char long_path[INFS_PATH_MAX + 1u];
    long_path[0] = '/';
    memset(long_path + 1, 'a', INFS_PATH_MAX - 1u);
    long_path[INFS_PATH_MAX] = '\0';
    expect(infs_get_attributes(&vol, long_path, &path_attributes) ==
               INFS_STATUS_NAME_TOO_LONG,
           "core path limit is enforced before traversal");

    const char first[] = "ABCDEFGHIJ";
    if (infs_write_file(&vol, "/alpha/data", first, sizeof(first) - 1u, 0) !=
        (int64_t)(sizeof(first) - 1u))
        fail("initial write");

    const char tail[] = "XYZ";
    uint64_t tail_off = INFS_BLOCK_SIZE * 2u + 3u;
    if (infs_write_file(&vol, "/alpha/data", tail, sizeof(tail) - 1u, tail_off) !=
        (int64_t)(sizeof(tail) - 1u))
        fail("sparse-style write");

    struct infs_attributes sparse_attributes;
    if (infs_get_attributes(&vol, "/alpha/data", &sparse_attributes) != 0)
        fail("get sparse attributes");
    expect(sparse_attributes.logical_size == tail_off + sizeof(tail) - 1u,
           "sparse write logical size");
    expect(sparse_attributes.allocated_size == 2u * INFS_BLOCK_SIZE,
           "sparse write allocates only touched blocks");

    unsigned char probe[32];
    memset(probe, 0xff, sizeof(probe));
    int64_t n = infs_read_file(&vol, "/alpha/data", probe, sizeof(probe),
                               INFS_BLOCK_SIZE + 100u);
    expect(n == (int64_t)sizeof(probe), "hole probe length");
    for (size_t i = 0; i < sizeof(probe); ++i)
        expect(probe[i] == 0, "unwritten gap must read as zero");

    if (infs_punch_hole(&vol, "/alpha/data",
                        2u * INFS_BLOCK_SIZE, INFS_BLOCK_SIZE) != 0)
        fail("punch sparse tail block");
    if (infs_get_attributes(&vol, "/alpha/data", &sparse_attributes) != 0)
        fail("get punched attributes");
    expect(sparse_attributes.allocated_size == INFS_BLOCK_SIZE,
           "full hole punch reclaims one block");

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

    if (infs_set_posix_compat(&vol, "/alpha/data",
                              INFS_POSIX_SET_PERMISSIONS, 0600, 0, 0) != 0)
        fail("chmod");
    struct infs_attributes attributes;
    if (infs_get_attributes(&vol, "/alpha/data", &attributes) != 0)
        fail("get attributes");
    expect(attributes.posix_permissions == 0600, "chmod persisted");
    expect(attributes.logical_size == 10, "size after regrow");
    expect(memcmp(attributes.object_id, stable_id, 16) == 0,
           "object identity stable across CoW updates");

    if (infs_rename(&vol, "/alpha/data", "/beta/moved") != 0)
        fail("cross-directory rename");
    expect(infs_get_attributes(&vol, "/alpha/data", &attributes) ==
               INFS_STATUS_NOT_FOUND,
           "old name removed");
    if (infs_get_attributes(&vol, "/beta/moved", &attributes) != 0)
        fail("new name lookup");
    expect(memcmp(attributes.object_id, stable_id, 16) == 0,
           "object identity stable across rename");

    if (infs_unlink(&vol, "/beta/moved") != 0)
        fail("unlink");
    if (infs_rmdir(&vol, "/alpha") != 0)
        fail("rmdir alpha");
    if (infs_rmdir(&vol, "/beta") != 0)
        fail("rmdir beta");

    infs_volume_close(&vol);

    if (infs_posix_volume_open(&vol, argv[1], 0) != 0)
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
