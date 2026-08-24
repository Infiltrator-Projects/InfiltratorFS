// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_VOLUME_H
#define INFILFS_VOLUME_H

#include <stddef.h>
#include <stdint.h>

#include "infilfs/format.h"
#include "infilfs/storage.h"

#define INFS_PATH_MAX 4096u
#define INFS_DEFAULT_DEFERRED_PUBLISH_BYTES \
    (UINT64_C(16) * 1024u * 1024u)

#define INFS_POSIX_SET_PERMISSIONS UINT32_C(0x00000001)
#define INFS_POSIX_SET_UID         UINT32_C(0x00000002)
#define INFS_POSIX_SET_GID         UINT32_C(0x00000004)

#define INFS_TIME_OMIT UINT32_C(0)
#define INFS_TIME_NOW  UINT32_C(1)
#define INFS_TIME_SET  UINT32_C(2)

struct infs_create_options {
    uint64_t portable_flags;
    uint32_t posix_permissions;
    uint32_t posix_uid;
    uint32_t posix_gid;
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

struct infs_time_update {
    uint32_t access_action;
    uint32_t modification_action;
    int64_t access_time_ns;
    int64_t modification_time_ns;
};

struct infs_deferred_range {
    uint64_t start;
    uint64_t count;
};

struct infs_volume {
    struct infs_storage storage;
    int writable;
    int checkpoint_repair_needed;
    /* Nonzero after the commit checkpoint may have reached storage but its
     * durability could not be established. No further mutation is safe until
     * the volume is closed and recovered from its physical checkpoints. */
    infs_status reopen_required_status;
    uint64_t size_bytes;
    struct infs_superblock_disk sb;
    uint8_t *bitmap;
    size_t bitmap_bytes;

    /* Transaction state. Adapters may deliberately leave a transaction open
     * across buffered mutations and publish it with infs_volume_sync(). */
    int tx_active;
    infs_status tx_error;
    struct infs_superblock_disk tx_base_sb;
    uint8_t *tx_base_bitmap;
    struct infs_deferred_range *tx_deferred;
    size_t tx_deferred_count;
    size_t tx_deferred_capacity;

    /* Generic publication policy. With deferred publication enabled, normal
     * mutators remain in the active transaction until the approximate dirty
     * byte threshold is reached. Explicit infs_volume_sync() always publishes
     * immediately. This is an adapter policy, not an on-disk format feature. */
    int deferred_publish;
    uint64_t deferred_publish_threshold_bytes;
    uint64_t tx_pending_bytes;

    /* Runtime-only performance hints. These never appear on disk and are
     * shared by every adapter. allocation_cursor makes allocation next-fit
     * instead of restarting at block 1, while object_cache accelerates stable
     * object-id to block lookups for the paged index. */
    uint64_t allocation_cursor;
    struct infs_object_cache_entry *object_cache;
    size_t object_cache_slots;
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

infs_status infs_volume_open_storage(struct infs_volume *vol,
                                     struct infs_storage *storage,
                                     int writable);
void infs_volume_close(struct infs_volume *vol);
infs_status infs_volume_sync(struct infs_volume *vol);
/* Enable or disable bounded deferred publication for normal mutators.
 * threshold_bytes == 0 selects INFS_DEFAULT_DEFERRED_PUBLISH_BYTES.
 * Explicit infs_volume_sync() remains a forced durability boundary. */
infs_status infs_volume_set_deferred_publish(struct infs_volume *vol,
                                             int enabled,
                                             uint64_t threshold_bytes);

infs_status infs_lookup_path(struct infs_volume *vol, const char *path,
                             struct infs_lookup *out);
infs_status infs_get_attributes(struct infs_volume *vol, const char *path,
                                struct infs_attributes *attributes);
infs_status infs_list_dir(struct infs_volume *vol, const char *path,
                          struct infs_dir_item **items, size_t *count);
void infs_free_dir_items(struct infs_dir_item *items);

infs_status infs_create_file(struct infs_volume *vol, const char *path,
                             const struct infs_create_options *options);
infs_status infs_mkdir(struct infs_volume *vol, const char *path,
                       const struct infs_create_options *options);
infs_status infs_create_symlink(struct infs_volume *vol, const char *path,
                                const char *target,
                                const struct infs_create_options *options);
infs_status infs_read_symlink(struct infs_volume *vol, const char *path,
                              char *target, size_t capacity,
                              size_t *length_out);
infs_status infs_unlink(struct infs_volume *vol, const char *path);
infs_status infs_rmdir(struct infs_volume *vol, const char *path);
infs_status infs_rename(struct infs_volume *vol, const char *oldpath,
                        const char *newpath);

int64_t infs_read_file(struct infs_volume *vol, const char *path, void *buf,
                       size_t size, uint64_t offset);
/* Durable convenience write: the successful call publishes the transaction. */
int64_t infs_write_file(struct infs_volume *vol, const char *path,
                        const void *buf, size_t size, uint64_t offset);
/* Adapter-oriented write: with deferred publication enabled, successful calls
 * remain in the active transaction until the configured threshold or an
 * explicit infs_volume_sync(); otherwise this behaves like a durable write. */
int64_t infs_write_file_buffered(struct infs_volume *vol, const char *path,
                                 const void *buf, size_t size,
                                 uint64_t offset);
infs_status infs_truncate_file(struct infs_volume *vol, const char *path,
                               uint64_t size);
infs_status infs_punch_hole(struct infs_volume *vol, const char *path,
                            uint64_t offset, uint64_t length);
infs_status infs_reflink_file(struct infs_volume *vol, const char *source_path,
                              const char *destination_path);

infs_status infs_set_posix_compat(struct infs_volume *vol, const char *path,
                                  uint32_t mask, uint32_t permissions,
                                  uint32_t uid, uint32_t gid);
infs_status infs_set_times(struct infs_volume *vol, const char *path,
                           const struct infs_time_update *update);

infs_status infs_scrub(struct infs_volume *vol,
                       struct infs_scrub_report *report);

#endif
