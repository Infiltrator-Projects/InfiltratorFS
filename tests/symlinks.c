// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/endian.h"
#include "infilfs/format.h"
#include "infilfs/format_volume.h"
#include "infilfs/storage.h"
#include "infilfs/volume.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_BLOCKS UINT64_C(4096)
#define TEST_SIZE ((size_t)(TEST_BLOCKS * INFS_BLOCK_SIZE))

struct memory_image {
    uint8_t *bytes;
    size_t size;
    uint64_t random_state;
};

static void fail(const char *message)
{
    fprintf(stderr, "symlinks: %s\n", message);
    exit(1);
}

static void expect(int condition, const char *message)
{
    if (!condition)
        fail(message);
}

static infs_status memory_read(void *context, uint64_t offset,
                               void *buffer, size_t size)
{
    struct memory_image *image = context;
    if (offset > image->size || size > image->size - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
    memcpy(buffer, image->bytes + (size_t)offset, size);
    return INFS_STATUS_OK;
}

static infs_status memory_write(void *context, uint64_t offset,
                                const void *buffer, size_t size)
{
    struct memory_image *image = context;
    if (offset > image->size || size > image->size - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
    memcpy(image->bytes + (size_t)offset, buffer, size);
    return INFS_STATUS_OK;
}

static infs_status memory_flush(void *context)
{
    (void)context;
    return INFS_STATUS_OK;
}

static infs_status memory_size(void *context, uint64_t *size_bytes,
                               int *is_device)
{
    struct memory_image *image = context;
    *size_bytes = image->size;
    *is_device = 0;
    return INFS_STATUS_OK;
}

static infs_status memory_random(void *context, void *buffer, size_t size)
{
    struct memory_image *image = context;
    uint8_t *out = buffer;
    for (size_t i = 0; i < size; ++i) {
        image->random_state ^= image->random_state << 13;
        image->random_state ^= image->random_state >> 7;
        image->random_state ^= image->random_state << 17;
        out[i] = (uint8_t)image->random_state;
    }
    return INFS_STATUS_OK;
}

static infs_status memory_time(void *context, int64_t *time_ns)
{
    (void)context;
    *time_ns = INT64_C(1787288400000000000);
    return INFS_STATUS_OK;
}

static void memory_close(void *context)
{
    (void)context;
}

static const struct infs_storage_ops memory_ops = {
    .read_at = memory_read,
    .write_at = memory_write,
    .flush = memory_flush,
    .get_size = memory_size,
    .random_bytes = memory_random,
    .current_time_ns = memory_time,
    .close = memory_close,
};

static struct infs_storage make_storage(struct memory_image *image)
{
    struct infs_storage storage = {
        .ops = &memory_ops,
        .context = image,
    };
    return storage;
}

static void expect_target(struct infs_volume *volume, const char *path,
                          const char *expected)
{
    char target[INFS_SYMLINK_TARGET_MAX + 1u];
    size_t length = 0;
    expect(infs_read_symlink(volume, path, NULL, 0, &length) == INFS_STATUS_OK,
           "query target length");
    expect(length == strlen(expected), "target length matches");
    expect(infs_read_symlink(volume, path, target, length, &length) ==
               INFS_STATUS_OVERFLOW,
           "target buffer requires trailing NUL");
    expect(infs_read_symlink(volume, path, target, sizeof(target), &length) ==
               INFS_STATUS_OK,
           "read target");
    expect(strcmp(target, expected) == 0, "target bytes match");
}

int main(void)
{
    struct memory_image image = {
        .bytes = calloc(1, TEST_SIZE),
        .size = TEST_SIZE,
        .random_state = UINT64_C(0x9e3779b97f4a7c15),
    };
    expect(image.bytes != NULL, "allocate image");

    struct infs_storage storage = make_storage(&image);
    expect(infs_format_storage(&storage, "symlink-test") == INFS_STATUS_OK,
           "format volume");

    struct infs_volume volume;
    storage = make_storage(&image);
    expect(infs_volume_open_storage(&volume, &storage, 1) == INFS_STATUS_OK,
           "open writable volume");
    expect(infs_le16_to_cpu(volume.sb.format_minor) == 9u,
           "Format minor is 0.9");
    expect((infs_le64_to_cpu(volume.sb.incompat_flags) &
            INFS_INCOMPAT_SYMBOLIC_LINKS) != 0,
           "symbolic-link feature enabled");

    const struct infs_create_options options = {
        .posix_permissions = 0777,
        .posix_uid = 1000,
        .posix_gid = 1001,
    };
    expect(infs_create_symlink(&volume, "/relative", "dir/file", &options) ==
               INFS_STATUS_OK,
           "create relative symbolic link");
    expect(infs_create_symlink(&volume, "/absolute", "/dir/file", NULL) ==
               INFS_STATUS_OK,
           "create absolute symbolic link");
    expect_target(&volume, "/relative", "dir/file");
    expect_target(&volume, "/absolute", "/dir/file");

    struct infs_attributes attributes;
    expect(infs_get_attributes(&volume, "/relative", &attributes) ==
               INFS_STATUS_OK,
           "stat symbolic link");
    expect(attributes.object_type == INFS_OBJECT_SYMLINK,
           "stat reports symbolic link");
    expect(attributes.logical_size == strlen("dir/file") &&
           attributes.link_count == 1 &&
           attributes.posix_permissions == 0777 &&
           attributes.posix_uid == 1000 && attributes.posix_gid == 1001,
           "symbolic-link metadata is correct");

    struct infs_dir_item *items = NULL;
    size_t count = 0;
    expect(infs_list_dir(&volume, "/", &items, &count) == INFS_STATUS_OK,
           "list symbolic links");
    expect(count == 2, "directory contains both symbolic links");
    expect(items[0].type == INFS_OBJECT_SYMLINK &&
           items[1].type == INFS_OBJECT_SYMLINK,
           "directory records symbolic-link type");
    infs_free_dir_items(items);

    expect(infs_rename(&volume, "/relative", "/renamed") == INFS_STATUS_OK,
           "rename symbolic link");
    expect_target(&volume, "/renamed", "dir/file");
    expect(infs_create_file(&volume, "/replace", NULL) == INFS_STATUS_OK,
           "create replacement destination");
    expect(infs_rename(&volume, "/absolute", "/replace") == INFS_STATUS_OK,
           "replace regular file with symbolic link");
    expect_target(&volume, "/replace", "/dir/file");
    expect(infs_unlink(&volume, "/renamed") == INFS_STATUS_OK,
           "unlink symbolic link");

    expect(infs_create_symlink(&volume, "/empty", "", NULL) ==
               INFS_STATUS_INVALID_ARGUMENT,
           "reject empty target");
    const char invalid_utf8[] = {(char)0xc0, (char)0x80, '\0'};
    expect(infs_create_symlink(&volume, "/bad", invalid_utf8, NULL) ==
               INFS_STATUS_INVALID_ARGUMENT,
           "reject invalid UTF-8 target");

    struct infs_scrub_report report;
    expect(infs_scrub(&volume, &report) == INFS_STATUS_OK,
           "scrub symbolic-link volume");
    expect(report.checksum_errors == 0 && report.metadata_errors == 0,
           "scrub reports symbolic-link volume clean");

    struct infs_lookup lookup;
    expect(infs_lookup_path(&volume, "/replace", &lookup) == INFS_STATUS_OK,
           "locate symbolic link for corruption test");
    uint8_t *object = image.bytes + lookup.block * INFS_BLOCK_SIZE;
    struct infs_symlink_payload_disk *payload =
        (struct infs_symlink_payload_disk *)(
            object + sizeof(struct infs_object_header_disk));
    uint8_t *stored_target = (uint8_t *)(payload + 1);
    uint8_t saved = stored_target[0];
    stored_target[0] = 0;
    expect(infs_scrub(&volume, &report) == INFS_STATUS_OK &&
           report.metadata_errors != 0,
           "scrub reports corrupted symbolic-link target");
    stored_target[0] = saved;
    expect(infs_scrub(&volume, &report) == INFS_STATUS_OK,
           "restored symbolic-link object scrubs clean");
    infs_volume_close(&volume);

    storage = make_storage(&image);
    expect(infs_volume_open_storage(&volume, &storage, 0) == INFS_STATUS_OK,
           "reopen symbolic-link volume");
    expect_target(&volume, "/replace", "/dir/file");
    expect(infs_get_attributes(&volume, "/renamed", &attributes) ==
               INFS_STATUS_NOT_FOUND,
           "unlinked symbolic link remains absent");
    infs_volume_close(&volume);

    free(image.bytes);
    puts("symbolic links: ok");
    return 0;
}
