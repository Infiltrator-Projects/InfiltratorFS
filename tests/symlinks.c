// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/endian.h"
#include "infilfs/format.h"
#include "infilfs/format_volume.h"
#include "infilfs/fs.h"
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
    fprintf(stderr, "namespace-links: %s\n", message);
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

static infs_status memory_time(void *context, struct infs_timestamp *time)
{
    (void)context;
    time->seconds = INT64_C(1787288400);
    time->nanoseconds = UINT32_C(0);
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
    .current_time = memory_time,
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
    expect(infs_le16_to_cpu(volume.sb.format_minor) == INFS_FORMAT_MINOR,
           "current Format minor");
    expect((infs_le64_to_cpu(volume.sb.incompat_flags) &
            INFS_INCOMPAT_SYMBOLIC_LINKS) != 0,
           "symbolic-link feature enabled");
    expect((infs_le64_to_cpu(volume.sb.incompat_flags) &
            INFS_INCOMPAT_HARD_LINKS) != 0,
           "hard-link feature enabled");
    expect((infs_le64_to_cpu(volume.sb.incompat_flags) &
            INFS_INCOMPAT_SNAPSHOTS) != 0,
           "snapshot feature enabled");

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

    static const uint8_t original_data[] = "shared-file-content";
    static const uint8_t changed_data[] = "updated-through-link";
    expect(infs_create_file(&volume, "/file", NULL) == INFS_STATUS_OK,
           "create hard-link source");
    expect(infs_write_file(&volume, "/file", original_data,
                           sizeof(original_data), 0) ==
               (int64_t)sizeof(original_data),
           "write hard-link source");
    expect(infs_link_file(&volume, "/file", "/alias") == INFS_STATUS_OK,
           "create regular-file hard link");
    struct infs_attributes file_attributes;
    struct infs_attributes alias_attributes;
    expect(infs_get_attributes(&volume, "/file", &file_attributes) ==
               INFS_STATUS_OK &&
           infs_get_attributes(&volume, "/alias", &alias_attributes) ==
               INFS_STATUS_OK,
           "stat both hard links");
    expect(memcmp(file_attributes.object_id, alias_attributes.object_id, 16) ==
               0 && file_attributes.link_count == 2u &&
           alias_attributes.link_count == 2u,
           "hard links share identity and reference count");
    expect(infs_write_file(&volume, "/alias", changed_data,
                           sizeof(changed_data), 0) ==
               (int64_t)sizeof(changed_data),
           "write through hard link");
    uint8_t linked_readback[sizeof(changed_data)];
    expect(infs_read_file(&volume, "/file", linked_readback,
                          sizeof(linked_readback), 0) ==
               (int64_t)sizeof(linked_readback) &&
           memcmp(linked_readback, changed_data, sizeof(changed_data)) == 0,
           "hard-link write is visible through original name");
    expect(infs_link_file(&volume, "/replace", "/bad-link") ==
               INFS_STATUS_NOT_SUPPORTED,
           "reject hard link to symbolic link");
    expect(infs_link_file(&volume, "/", "/bad-directory-link") ==
               INFS_STATUS_IS_DIRECTORY,
           "reject hard link to directory");
    expect(infs_unlink(&volume, "/file") == INFS_STATUS_OK,
           "unlink one hard-link name");
    expect(infs_get_attributes(&volume, "/alias", &alias_attributes) ==
               INFS_STATUS_OK && alias_attributes.link_count == 1u,
           "remaining hard link retains object");
    expect(infs_mkdir(&volume, "/links", NULL) == INFS_STATUS_OK,
           "create cross-directory hard-link parent");
    expect(infs_link_file(&volume, "/alias", "/links/second") ==
               INFS_STATUS_OK,
           "create cross-directory hard link");
    expect(infs_rename(&volume, "/links/second", "/moved") ==
               INFS_STATUS_OK,
           "rename one hard-link name across directories");
    expect(infs_unlink(&volume, "/alias") == INFS_STATUS_OK,
           "unlink original cross-directory name");
    expect(infs_get_attributes(&volume, "/moved", &alias_attributes) ==
               INFS_STATUS_OK && alias_attributes.link_count == 1u,
           "renamed hard link remains reachable");

    static const uint8_t victim_data[] = "replacement-victim";
    expect(infs_create_file(&volume, "/victim", NULL) == INFS_STATUS_OK &&
           infs_write_file(&volume, "/victim", victim_data,
                           sizeof(victim_data), 0) ==
               (int64_t)sizeof(victim_data),
           "create hard-linked replacement victim");
    expect(infs_link_file(&volume, "/victim", "/victim-alias") ==
               INFS_STATUS_OK,
           "link replacement victim");
    expect(infs_rename(&volume, "/moved", "/victim") == INFS_STATUS_OK,
           "replace one name of a hard-linked file");
    uint8_t victim_readback[sizeof(victim_data)];
    expect(infs_read_file(&volume, "/victim-alias", victim_readback,
                          sizeof(victim_readback), 0) ==
               (int64_t)sizeof(victim_readback) &&
           memcmp(victim_readback, victim_data, sizeof(victim_data)) == 0,
           "replacement victim survives through its other name");
    expect(infs_get_attributes(&volume, "/victim-alias", &alias_attributes) ==
               INFS_STATUS_OK && alias_attributes.link_count == 1u,
           "replacement decrements victim reference count");

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

    expect(infs_lookup_path(&volume, "/victim", &lookup) == INFS_STATUS_OK,
           "locate hard-linked object for corruption test");
    object = image.bytes + lookup.block * INFS_BLOCK_SIZE;
    struct infs_file_payload_disk *corrupt_file =
        (struct infs_file_payload_disk *)(
            object + sizeof(struct infs_object_header_disk));
    uint64_t saved_links = corrupt_file->attributes.link_count;
    corrupt_file->attributes.link_count = infs_cpu_to_le64(2u);
    expect(infs_object_finalize(object) == INFS_STATUS_OK,
           "finalize mismatched hard-link count");
    expect(infs_scrub(&volume, &report) == INFS_STATUS_OK &&
           report.metadata_errors != 0,
           "scrub reports mismatched hard-link count");
    corrupt_file->attributes.link_count = saved_links;
    expect(infs_object_finalize(object) == INFS_STATUS_OK &&
           infs_scrub(&volume, &report) == INFS_STATUS_OK,
           "restored hard-link count scrubs clean");
    infs_volume_close(&volume);

    storage = make_storage(&image);
    expect(infs_volume_open_storage(&volume, &storage, 0) == INFS_STATUS_OK,
           "reopen symbolic-link volume");
    expect_target(&volume, "/replace", "/dir/file");
    expect(infs_get_attributes(&volume, "/victim", &alias_attributes) ==
               INFS_STATUS_OK && alias_attributes.link_count == 1u,
           "hard-link object persists across remount");
    expect(infs_get_attributes(&volume, "/renamed", &attributes) ==
               INFS_STATUS_NOT_FOUND,
           "unlinked symbolic link remains absent");
    infs_volume_close(&volume);

    free(image.bytes);
    puts("symbolic and hard links: ok");
    return 0;
}
