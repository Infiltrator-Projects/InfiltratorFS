// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/volume.h"
#include "infilfs/fs.h"
#include "infilfs/posix_io.h"

#include "infilfs/endian.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s <image> ls [path]\n"
        "  %s <image> stat <path>\n"
        "  %s <image> mkdir <path>\n"
        "  %s <image> put <host-file> <path>\n"
        "  %s <image> write <host-file> <path> <offset>\n"
        "  %s <image> punch <path> <offset> <length>\n"
        "  %s <image> resize <bytes|KiB|MiB|GiB|TiB|max>\n"
        "  %s <image> cat <path>\n"
        "  %s <image> map <path> <logical-block>\n"
        "  %s <image> reflink <source-path> <destination-path>\n"
        "  %s <image> symlink <target> <path>\n"
        "  %s <image> readlink <path>\n"
        "  %s <image> link <existing-path> <new-path>\n"
        "  %s <image> mv <old-path> <new-path>\n"
        "  %s <image> rm <path>\n"
        "  %s <image> rmdir <path>\n"
        "  %s <image> snapshot-create <name>\n"
        "  %s <image> snapshot-list\n"
        "  %s <image> snapshot-delete <name>\n"
        "  %s <image> snapshot-ls <name> [path]\n"
        "  %s <image> snapshot-stat <name> <path>\n"
        "  %s <image> snapshot-cat <name> <path>\n"
        "  %s <image> snapshot-readlink <name> <path>\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
        prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

static int die_errno(const char *what)
{
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
    return 1;
}

static int die_status(const char *what, infs_status status)
{
    fprintf(stderr, "%s: %s\n", what, infs_status_string(status));
    return 1;
}

static int command_ls(struct infs_volume *vol, const char *path)
{
    struct infs_dir_item *items = NULL;
    size_t count = 0;
    infs_status status = infs_list_dir(vol, path, &items, &count);
    if (status != INFS_STATUS_OK)
        return die_status("ls", status);
    for (size_t i = 0; i < count; ++i)
        printf("%c %s\n",
               items[i].type == INFS_OBJECT_DIRECTORY ? 'd' :
               items[i].type == INFS_OBJECT_SYMLINK ? 'l' : '-',
               items[i].name);
    infs_free_dir_items(items);
    return 0;
}

static int command_stat(struct infs_volume *vol, const char *path)
{
    struct infs_attributes attributes;
    infs_status status = infs_get_attributes(vol, path, &attributes);
    if (status != INFS_STATUS_OK)
        return die_status("stat", status);
    printf("type=%u mode=%#o uid=%u gid=%u links=%" PRIu64
           " size=%" PRIu64 " allocated=%" PRIu64 "\n",
           attributes.object_type, attributes.posix_permissions,
           attributes.posix_uid, attributes.posix_gid,
           attributes.link_count, attributes.logical_size,
           attributes.allocated_size);
    return 0;
}

static int command_put(struct infs_volume *vol, const char *host, const char *dest)
{
    int input = open(host, O_RDONLY | O_CLOEXEC);
    if (input < 0)
        return die_errno("open host file");

    struct infs_attributes attributes;
    infs_status status = infs_get_attributes(vol, dest, &attributes);
    if (status != INFS_STATUS_OK) {
        if (status != INFS_STATUS_NOT_FOUND) {
            close(input);
            return die_status("lookup destination", status);
        }
        const struct infs_create_options options = {
            .posix_permissions = 0644,
            .posix_uid = (uint32_t)getuid(),
            .posix_gid = (uint32_t)getgid(),
        };
        status = infs_create_file(vol, dest, &options);
        if (status != INFS_STATUS_OK) {
            close(input);
            return die_status("create destination", status);
        }
    } else if (attributes.object_type != INFS_OBJECT_FILE) {
        close(input);
        return die_status("destination", INFS_STATUS_IS_DIRECTORY);
    }

    status = infs_truncate_file(vol, dest, 0);
    if (status != INFS_STATUS_OK) {
        close(input);
        return die_status("truncate destination", status);
    }

    uint8_t buf[256 * 1024];
    uint64_t off = 0;
    for (;;) {
        ssize_t n = read(input, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(input);
            return die_errno("read host file");
        }
        if (n == 0)
            break;
        int64_t w = infs_write_file(vol, dest, buf, (size_t)n, off);
        if (w < 0 || w != n) {
            close(input);
            return die_status("write destination",
                              w < 0 ? (infs_status)w : INFS_STATUS_IO_ERROR);
        }
        off += w;
    }
    close(input);
    return 0;
}
