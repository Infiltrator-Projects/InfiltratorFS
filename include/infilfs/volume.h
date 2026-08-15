// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_VOLUME_H
#define INFILFS_VOLUME_H

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "infilfs/format.h"

struct infs_deferred_range {
    uint64_t start;
    uint64_t count;
};

struct infs_volume {
    int fd;
    int writable;
    uint64_t size_bytes;
    struct infs_superblock_disk sb;
    uint8_t *bitmap;
    size_t bitmap_bytes;

    /* Phase 2 transaction state. The public struct remains intentionally
     * inspectable while the prototype format is evolving. */
    int tx_active;
    struct infs_superblock_disk tx_base_sb;
    uint8_t *tx_base_bitmap;
    struct infs_deferred_range *tx_deferred;
    size_t tx_deferred_count;
    size_t tx_deferred_capacity;
};

struct infs_lookup {
    uint8_t object_id[16];
    uint64_t block;
    uint16_t type;
};


struct infs_scrub_report {
    uint64_t files_checked;
    uint64_t data_blocks_checked;
    uint64_t checksum_errors;
    uint64_t metadata_errors;
};

struct infs_dir_item {
    char name[INFS_NAME_MAX + 1u];
    uint8_t object_id[16];
    uint16_t type;
};

int infs_volume_open(struct infs_volume *vol, const char *path, int writable);
void infs_volume_close(struct infs_volume *vol);
int infs_volume_sync(struct infs_volume *vol);

int infs_lookup_path(struct infs_volume *vol, const char *path,
                     struct infs_lookup *out);
int infs_getattr(struct infs_volume *vol, const char *path, struct stat *st);
int infs_list_dir(struct infs_volume *vol, const char *path,
                  struct infs_dir_item **items, size_t *count);
void infs_free_dir_items(struct infs_dir_item *items);

int infs_create_file(struct infs_volume *vol, const char *path, mode_t mode,
                     uid_t uid, gid_t gid);
int infs_mkdir(struct infs_volume *vol, const char *path, mode_t mode,
               uid_t uid, gid_t gid);
int infs_unlink(struct infs_volume *vol, const char *path);
int infs_rmdir(struct infs_volume *vol, const char *path);
int infs_rename(struct infs_volume *vol, const char *oldpath,
                const char *newpath);

ssize_t infs_read_file(struct infs_volume *vol, const char *path, void *buf,
                       size_t size, off_t offset);
ssize_t infs_write_file(struct infs_volume *vol, const char *path,
                        const void *buf, size_t size, off_t offset);
int infs_truncate_file(struct infs_volume *vol, const char *path, off_t size);

int infs_chmod(struct infs_volume *vol, const char *path, mode_t mode);
int infs_chown(struct infs_volume *vol, const char *path, uid_t uid, gid_t gid);
int infs_utimens(struct infs_volume *vol, const char *path,
                 const struct timespec tv[2]);

int infs_scrub(struct infs_volume *vol, struct infs_scrub_report *report);

#endif
