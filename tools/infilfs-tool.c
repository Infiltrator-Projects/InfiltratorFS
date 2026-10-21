// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/volume.h"
#include "infilfs/fs.h"
#include "infilfs/posix_io.h"

#include "infilfs/endian.h"
#include "infiltratr/core.h"
#include "infiltratr/posix_io.h"
#include "infiltratr/quantity.h"
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
        "  %s <image> snapshot-rollback <name>\n"
        "  %s <image> snapshot-ls <name> [path]\n"
        "  %s <image> snapshot-stat <name> <path>\n"
        "  %s <image> snapshot-cat <name> <path>\n"
        "  %s <image> snapshot-readlink <name> <path>\n"
        "  %s <image> snapshot-restore <name> <source-path> <destination-path> [--replace]\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
        prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
        prog, prog);
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

static int command_write(struct infs_volume *vol, const char *host,
                         const char *dest, const char *offset_text)
{
    uint64_t off64 = 0;
    if (!infiltratr_parse_u64_range(
            offset_text, 0, 0, (uint64_t)INT64_MAX, &off64)) {
        errno = EINVAL;
        return die_errno("write offset");
    }
    int input = open(host, O_RDONLY | O_CLOEXEC);
    if (input < 0)
        return die_errno("open host file");
    struct stat st;
    if (fstat(input, &st) != 0 || st.st_size < 0 || (uintmax_t)st.st_size > SIZE_MAX) {
        close(input);
        if (errno == 0) errno = EFBIG;
        return die_errno("host file size");
    }
    size_t size = (size_t)st.st_size;
    uint8_t *buf = size ? malloc(size) : NULL;
    if (size && !buf) {
        close(input);
        return die_errno("allocate host buffer");
    }
    if (size && infiltratr_read_full(input, buf, size) != 0) {
        free(buf);
        close(input);
        return die_errno("read host file");
    }
    close(input);
    int64_t written = infs_write_file(vol, dest, buf, size, (uint64_t)off64);
    free(buf);
    if (written < 0 || (size_t)written != size) {
        return die_status("write destination",
                          written < 0 ? (infs_status)written :
                                        INFS_STATUS_IO_ERROR);
    }
    return 0;
}

static int parse_resize_size(struct infs_volume *vol, const char *text,
                             uint64_t *bytes_out)
{
    uint64_t storage_bytes = 0;
    int is_device = 0;
    if (!vol || !text || !bytes_out)
        return 0;
    if (strcmp(text, "max") == 0) {
        if (infs_storage_get_size(&vol->storage, &storage_bytes,
                                  &is_device) != INFS_STATUS_OK)
            return 0;
        (void)is_device;
        if ((storage_bytes % INFS_BLOCK_SIZE) != 0)
            return 0;
        *bytes_out = storage_bytes;
        return 1;
    }

    uint64_t bytes = 0;
    if (!infiltratr_parse_binary_quantity_u64(text, &bytes))
        return 0;
    if ((bytes % INFS_BLOCK_SIZE) != 0)
        return 0;
    *bytes_out = bytes;
    return 1;
}

static int command_resize(struct infs_volume *vol, const char *size_text)
{
    uint64_t bytes = 0;
    if (!parse_resize_size(vol, size_text, &bytes)) {
        errno = EINVAL;
        return die_errno("resize size");
    }
    uint64_t before =
        infs_le64_to_cpu(vol->sb.total_blocks) * (uint64_t)INFS_BLOCK_SIZE;
    infs_status status = infs_volume_resize(vol, bytes);
    if (status != INFS_STATUS_OK)
        return die_status("resize", status);
    printf("resized=%" PRIu64 " previous=%" PRIu64 "\n", bytes, before);
    return 0;
}

static int command_map(struct infs_volume *vol, const char *path,
                       const char *logical_text)
{
    uint64_t logical = 0;
    if (!infiltratr_parse_u64(logical_text, 0, &logical)) {
        errno = EINVAL;
        return die_errno("map logical block");
    }
    struct infs_lookup lk;
    infs_status status = infs_lookup_path(vol, path, &lk);
    if (status != INFS_STATUS_OK)
        return die_status("map lookup", status);
    if (lk.type != INFS_OBJECT_FILE)
        return die_status("map", INFS_STATUS_IS_DIRECTORY);
    uint8_t object[INFS_BLOCK_SIZE];
    status = infs_storage_read(&vol->storage, lk.block * INFS_BLOCK_SIZE,
                               object, sizeof(object));
    if (status != INFS_STATUS_OK)
        return die_status("map metadata", status);
    if (!infs_validate_object_block(object))
        return die_status("map metadata", INFS_STATUS_CORRUPT);
    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)object;
    struct infs_file_payload_disk *file =
        (struct infs_file_payload_disk *)(object + sizeof(*hdr));
    uint32_t count = infs_le32_to_cpu(file->extent_count);
    struct infs_extent_disk *ext = (struct infs_extent_disk *)(file + 1);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t l = infs_le64_to_cpu(ext[i].logical_block);
        uint32_t n = infs_le32_to_cpu(ext[i].block_count);
        uint64_t logical_end = 0;
        if (!infiltratr_u64_add_checked(
                l, (uint64_t)n, &logical_end))
            return die_status("map metadata", INFS_STATUS_CORRUPT);
        if (logical >= l && logical < logical_end) {
            if (infs_le32_to_cpu(ext[i].flags) == INFS_EXTENT_HOLE) {
                puts("hole");
                return 0;
            }
            uint64_t physical = 0;
            if (!infiltratr_u64_add_checked(
                    infs_le64_to_cpu(ext[i].physical_block),
                    logical - l, &physical))
                return die_status("map metadata", INFS_STATUS_CORRUPT);
            printf("%" PRIu64 "\n", physical);
            return 0;
        }
    }
    return die_status("map", INFS_STATUS_NOT_FOUND);
}

static int command_punch(struct infs_volume *vol, const char *path,
                         const char *offset_text, const char *length_text)
{
    uint64_t offset = 0;
    uint64_t length = 0;
    if (!infiltratr_parse_u64(offset_text, 0, &offset)) {
        errno = EINVAL;
        return die_errno("punch offset");
    }
    if (!infiltratr_parse_u64(length_text, 0, &length)) {
        errno = EINVAL;
        return die_errno("punch length");
    }
    infs_status status = infs_punch_hole(vol, path, offset, length);
    return status == INFS_STATUS_OK ? 0 : die_status("punch", status);
}

static int command_cat(struct infs_volume *vol, const char *path)
{
    struct infs_attributes attributes;
    infs_status status = infs_get_attributes(vol, path, &attributes);
    if (status != INFS_STATUS_OK)
        return die_status("cat", status);
    if (attributes.object_type != INFS_OBJECT_FILE)
        return die_status("cat", INFS_STATUS_IS_DIRECTORY);

    uint8_t buf[256 * 1024];
    uint64_t off = 0;
    while (off < attributes.logical_size) {
        size_t want = sizeof(buf);
        if ((uint64_t)want > attributes.logical_size - off)
            want = (size_t)(attributes.logical_size - off);
        int64_t n = infs_read_file(vol, path, buf, want, off);
        if (n < 0)
            return die_status("read file", (infs_status)n);
        if (n == 0)
            return die_status("short read", INFS_STATUS_IO_ERROR);
        if (infiltratr_write_full(
                STDOUT_FILENO, buf, (size_t)n) != 0)
            return die_errno("stdout");
        off += n;
    }
    return 0;
}

static int command_readlink(struct infs_volume *vol, const char *path)
{
    char target[INFS_SYMLINK_TARGET_MAX + 1u];
    size_t length = 0;
    infs_status status = infs_read_symlink(
        vol, path, target, sizeof(target), &length);
    if (status != INFS_STATUS_OK)
        return die_status("readlink", status);
    static const char newline = '\n';
    if (infiltratr_write_full(STDOUT_FILENO, target, length) != 0 ||
        infiltratr_write_full(STDOUT_FILENO, &newline, 1u) != 0)
        return die_errno("stdout");
    return 0;
}

static int command_snapshot_list(struct infs_volume *vol)
{
    struct infs_snapshot_info *snapshots = NULL;
    size_t count = 0;
    infs_status status = infs_snapshot_list(vol, &snapshots, &count);
    if (status != INFS_STATUS_OK)
        return die_status("snapshot-list", status);
    for (size_t i = 0; i < count; ++i)
        printf("%s generation=%" PRIu64 " created_seconds=%" PRId64
               " created_nanoseconds=%" PRIu32 "\n",
               snapshots[i].name, snapshots[i].generation,
               snapshots[i].created_time.seconds,
               snapshots[i].created_time.nanoseconds);
    infs_free_snapshot_infos(snapshots);
    return 0;
}

static int command_snapshot_ls(struct infs_volume *vol, const char *snapshot,
                               const char *path)
{
    struct infs_dir_item *items = NULL;
    size_t count = 0;
    infs_status status = infs_snapshot_list_dir(
        vol, snapshot, path, &items, &count);
    if (status != INFS_STATUS_OK)
        return die_status("snapshot-ls", status);
    for (size_t i = 0; i < count; ++i)
        printf("%c %s\n",
               items[i].type == INFS_OBJECT_DIRECTORY ? 'd' :
               items[i].type == INFS_OBJECT_SYMLINK ? 'l' : '-',
               items[i].name);
    infs_free_dir_items(items);
    return 0;
}

static int command_snapshot_stat(struct infs_volume *vol,
                                 const char *snapshot, const char *path)
{
    struct infs_attributes attributes;
    infs_status status = infs_snapshot_get_attributes(
        vol, snapshot, path, &attributes);
    if (status != INFS_STATUS_OK)
        return die_status("snapshot-stat", status);
    printf("type=%u mode=%#o uid=%u gid=%u links=%" PRIu64
           " size=%" PRIu64 " allocated=%" PRIu64 "\n",
           attributes.object_type, attributes.posix_permissions,
           attributes.posix_uid, attributes.posix_gid,
           attributes.link_count, attributes.logical_size,
           attributes.allocated_size);
    return 0;
}

static int command_snapshot_cat(struct infs_volume *vol,
                                const char *snapshot, const char *path)
{
    struct infs_attributes attributes;
    infs_status status = infs_snapshot_get_attributes(
        vol, snapshot, path, &attributes);
    if (status != INFS_STATUS_OK)
        return die_status("snapshot-cat", status);
    if (attributes.object_type != INFS_OBJECT_FILE)
        return die_status("snapshot-cat", INFS_STATUS_IS_DIRECTORY);
    uint8_t buf[256 * 1024];
    uint64_t off = 0;
    while (off < attributes.logical_size) {
        size_t want = sizeof(buf);
        if ((uint64_t)want > attributes.logical_size - off)
            want = (size_t)(attributes.logical_size - off);
        int64_t n = infs_snapshot_read_file(
            vol, snapshot, path, buf, want, off);
        if (n <= 0)
            return die_status("snapshot-cat",
                              n < 0 ? (infs_status)n : INFS_STATUS_IO_ERROR);
        if (infiltratr_write_full(
                STDOUT_FILENO, buf, (size_t)n) != 0)
            return die_errno("stdout");
        off += (uint64_t)n;
    }
    return 0;
}

static int command_snapshot_readlink(struct infs_volume *vol,
                                     const char *snapshot, const char *path)
{
    char target[INFS_SYMLINK_TARGET_MAX + 1u];
    size_t length = 0;
    infs_status status = infs_snapshot_read_symlink(
        vol, snapshot, path, target, sizeof(target), &length);
    if (status != INFS_STATUS_OK)
        return die_status("snapshot-readlink", status);
    static const char newline = '\n';
    if (infiltratr_write_full(STDOUT_FILENO, target, length) != 0 ||
        infiltratr_write_full(STDOUT_FILENO, &newline, 1u) != 0)
        return die_errno("stdout");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }
    const char *image = argv[1];
    const char *cmd = argv[2];
    int writable = strcmp(cmd, "ls") != 0 && strcmp(cmd, "stat") != 0 &&
        strcmp(cmd, "cat") != 0 && strcmp(cmd, "map") != 0 &&
        strcmp(cmd, "readlink") != 0 && strcmp(cmd, "snapshot-list") != 0 &&
        strcmp(cmd, "snapshot-ls") != 0 &&
        strcmp(cmd, "snapshot-stat") != 0 &&
        strcmp(cmd, "snapshot-cat") != 0 &&
        strcmp(cmd, "snapshot-readlink") != 0;

    struct infs_volume vol;
    infs_status status = infs_posix_volume_open(&vol, image, writable);
    if (status != INFS_STATUS_OK)
        return die_status("open InfiltratorFS", status);

    int rc = 0;
    if (strcmp(cmd, "ls") == 0) {
        if (argc > 4) rc = 2; else rc = command_ls(&vol, argc == 4 ? argv[3] : "/");
    } else if (strcmp(cmd, "stat") == 0 && argc == 4) {
        rc = command_stat(&vol, argv[3]);
    } else if (strcmp(cmd, "mkdir") == 0 && argc == 4) {
        const struct infs_create_options options = {
            .posix_permissions = 0755,
            .posix_uid = (uint32_t)getuid(),
            .posix_gid = (uint32_t)getgid(),
        };
        status = infs_mkdir(&vol, argv[3], &options);
        if (status != INFS_STATUS_OK) rc = die_status("mkdir", status);
    } else if (strcmp(cmd, "put") == 0 && argc == 5) {
        rc = command_put(&vol, argv[3], argv[4]);
    } else if (strcmp(cmd, "write") == 0 && argc == 6) {
        rc = command_write(&vol, argv[3], argv[4], argv[5]);
    } else if (strcmp(cmd, "punch") == 0 && argc == 6) {
        rc = command_punch(&vol, argv[3], argv[4], argv[5]);
    } else if (strcmp(cmd, "resize") == 0 && argc == 4) {
        rc = command_resize(&vol, argv[3]);
    } else if (strcmp(cmd, "cat") == 0 && argc == 4) {
        rc = command_cat(&vol, argv[3]);
    } else if (strcmp(cmd, "map") == 0 && argc == 5) {
        rc = command_map(&vol, argv[3], argv[4]);
    } else if (strcmp(cmd, "reflink") == 0 && argc == 5) {
        status = infs_reflink_file(&vol, argv[3], argv[4]);
        if (status != INFS_STATUS_OK) rc = die_status("reflink", status);
    } else if (strcmp(cmd, "symlink") == 0 && argc == 5) {
        const struct infs_create_options options = {
            .posix_permissions = 0777,
            .posix_uid = (uint32_t)getuid(),
            .posix_gid = (uint32_t)getgid(),
        };
        status = infs_create_symlink(&vol, argv[4], argv[3], &options);
        if (status != INFS_STATUS_OK) rc = die_status("symlink", status);
    } else if (strcmp(cmd, "readlink") == 0 && argc == 4) {
        rc = command_readlink(&vol, argv[3]);
    } else if (strcmp(cmd, "link") == 0 && argc == 5) {
        status = infs_link_file(&vol, argv[3], argv[4]);
        if (status != INFS_STATUS_OK) rc = die_status("link", status);
    } else if (strcmp(cmd, "mv") == 0 && argc == 5) {
        status = infs_rename(&vol, argv[3], argv[4]);
        if (status != INFS_STATUS_OK) rc = die_status("mv", status);
    } else if (strcmp(cmd, "rm") == 0 && argc == 4) {
        status = infs_unlink(&vol, argv[3]);
        if (status != INFS_STATUS_OK) rc = die_status("rm", status);
    } else if (strcmp(cmd, "rmdir") == 0 && argc == 4) {
        status = infs_rmdir(&vol, argv[3]);
        if (status != INFS_STATUS_OK) rc = die_status("rmdir", status);
    } else if (strcmp(cmd, "snapshot-create") == 0 && argc == 4) {
        status = infs_snapshot_create(&vol, argv[3]);
        if (status != INFS_STATUS_OK) rc = die_status("snapshot-create", status);
    } else if (strcmp(cmd, "snapshot-list") == 0 && argc == 3) {
        rc = command_snapshot_list(&vol);
    } else if (strcmp(cmd, "snapshot-delete") == 0 && argc == 4) {
        status = infs_snapshot_delete(&vol, argv[3]);
        if (status != INFS_STATUS_OK) rc = die_status("snapshot-delete", status);
    } else if (strcmp(cmd, "snapshot-rollback") == 0 && argc == 4) {
        status = infs_snapshot_rollback(&vol, argv[3]);
        if (status != INFS_STATUS_OK) rc = die_status("snapshot-rollback", status);
    } else if (strcmp(cmd, "snapshot-ls") == 0 &&
               (argc == 4 || argc == 5)) {
        rc = command_snapshot_ls(&vol, argv[3], argc == 5 ? argv[4] : "/");
    } else if (strcmp(cmd, "snapshot-stat") == 0 && argc == 5) {
        rc = command_snapshot_stat(&vol, argv[3], argv[4]);
    } else if (strcmp(cmd, "snapshot-cat") == 0 && argc == 5) {
        rc = command_snapshot_cat(&vol, argv[3], argv[4]);
    } else if (strcmp(cmd, "snapshot-readlink") == 0 && argc == 5) {
        rc = command_snapshot_readlink(&vol, argv[3], argv[4]);
    } else if (strcmp(cmd, "snapshot-restore") == 0 &&
               (argc == 6 || argc == 7)) {
        int replace = argc == 7 && strcmp(argv[6], "--replace") == 0;
        if (argc == 7 && !replace) {
            rc = 2;
        } else {
            status = infs_snapshot_restore_path(
                &vol, argv[3], argv[4], argv[5], replace);
            if (status != INFS_STATUS_OK)
                rc = die_status("snapshot-restore", status);
        }
    } else {
        usage(argv[0]);
        rc = 2;
    }

    infs_volume_close(&vol);
    return rc;
}
