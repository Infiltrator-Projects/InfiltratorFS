// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_VOLUME_H
#define INFILFS_VOLUME_H

#include <stddef.h>
#include <stdint.h>

#include "format.h"
#include "status.h"
#include "storage.h"

#define INFS_PATH_MAX 4096u

struct infs_lookup {
    uint8_t object_id[16];
    uint16_t type;
    uint64_t block;
};

struct infs_dir_item {
    char name[INFS_NAME_MAX + 1];
    uint8_t object_id[16];
    uint16_t type;
};

struct infs_attributes {
    uint8_t object_id[16];
    uint16_t object_type;
    uint64_t logical_size;
    uint64_t allocated_size;
    uint64_t link_count;
    uint64_t portable_flags;
    int64_t birth_time_ns;
    int64_t access_time_ns;
    int64_t modification_time_ns;
    int64_t change_time_ns;
    uint8_t security_object_id[16];
    uint8_t extended_attributes_object_id[16];
    uint32_t posix_permissions;
    uint32_t posix_uid;
    uint32_t posix_gid;
};

struct infs_create_options {
    uint64_t portable_flags;
    uint32_t posix_permissions;
    uint32_t posix_uid;
    uint32_t posix_gid;
};

struct infs_scrub_report {
    uint64_t files_checked;
    uint64_t data_blocks_checked;
    uint64_t checksum_errors;
    uint64_t metadata_errors;
};

struct infs_volume {
    struct infs_storage storage;
    struct infs_superblock_disk sb;
    uint8_t *bitmap;
    size_t bitmap_bytes;
    uint8_t *tx_base_bitmap;
    uint8_t *tx_reclaim_bitmap;
    uint64_t tx_base_free_blocks;
    int writable;
    int tx_active;
    int tx_committed;
    infs_status tx_error;
    int write_disabled;
};

infs_status infs_volume_open_storage(struct infs_volume *vol,
                                     struct infs_storage *storage,
                                     int writable);
void infs_volume_close(struct infs_volume *vol);
infs_status infs_volume_sync(struct infs_volume *vol);

infs_status infs_lookup_path(struct infs_volume *vol, const char *path,
                             struct infs_lookup *out);
infs_status infs_get_attributes(struct infs_volume *vol, const char *path,
                                struct infs_attributes *attributes);
infs_status infs_set_attributes(struct infs_volume *vol, const char *path,
                                uint64_t portable_flags,
                                uint32_t posix_permissions,
                                uint32_t posix_uid, uint32_t posix_gid);
infs_status infs_list_dir(struct infs_volume *vol, const char *path,
                          struct infs_dir_item **items, size_t *count);
void infs_free_dir_items(struct infs_dir_item *items);

infs_status infs_create_file(struct infs_volume *vol, const char *path,
                             const struct infs_create_options *options);
infs_status infs_mkdir(struct infs_volume *vol, const char *path,
                       const struct infs_create_options *options);
infs_status infs_unlink(struct infs_volume *vol, const char *path);
infs_status infs_rmdir(struct infs_volume *vol, const char *path);
infs_status infs_rename(struct infs_volume *vol, const char *oldpath,
                        const char *newpath);

int64_t infs_read_file(struct infs_volume *vol, const char *path, void *buf,
                       size_t size, uint64_t offset);
int64_t infs_write_file(struct infs_volume *vol, const char *path,
                        const void *buf, size_t size, uint64_t offset);
infs_status infs_truncate_file(struct infs_volume *vol, const char *path,
                               uint64_t size);
infs_status infs_punch_hole(struct infs_volume *vol, const char *path,
                            uint64_t offset, uint64_t length);
infs_status infs_reflink_file(struct infs_volume *vol, const char *source_path,
                              const char *destination_path);
infs_status infs_scrub(struct infs_volume *vol, struct infs_scrub_report *report);

#endif
