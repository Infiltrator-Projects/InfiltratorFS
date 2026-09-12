// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_VOLUME_H
#define INFILFS_VOLUME_H

#include <stddef.h>
#include <stdint.h>

#include "infilfs/format.h"
#include "infilfs/storage.h"
#include "infilfs/time.h"

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
    struct infs_timestamp birth_time;
    struct infs_timestamp access_time;
    struct infs_timestamp modification_time;
    struct infs_timestamp change_time;
    uint8_t security_object_id[16];
    uint8_t extended_attributes_object_id[16];
    uint32_t posix_permissions;
    uint32_t posix_uid;
    uint32_t posix_gid;
};

struct infs_time_update {
    uint32_t birth_action;
    uint32_t access_action;
    uint32_t modification_action;
    uint32_t change_action;
    struct infs_timestamp birth_time;
    struct infs_timestamp access_time;
    struct infs_timestamp modification_time;
    struct infs_timestamp change_time;
};

struct infs_deferred_range {
    uint64_t start;
    uint64_t count;
};

struct infs_free_extent;

struct infs_volume {
    struct infs_storage storage;
    int writable;
    int checkpoint_repair_needed;
    infs_status reopen_required_status;
    uint64_t size_bytes;
    struct infs_superblock_disk sb;
    uint8_t *bitmap;
    size_t bitmap_bytes;

    uint64_t *allocation_leaf_blocks;
    uint64_t *allocation_branch_blocks;
    size_t allocation_leaf_count;
    size_t allocation_branch_count;
    size_t allocation_level1_count;
    size_t allocation_level2_count;

    int tx_active;
    infs_status tx_error;
    struct infs_superblock_disk tx_base_sb;
    struct infs_deferred_range *tx_deferred;
    size_t tx_deferred_count;
    size_t tx_deferred_capacity;
    struct infs_deferred_range *tx_allocated;
    size_t tx_allocated_count;
    size_t tx_allocated_capacity;

    int tx_operation_active;
    struct infs_superblock_disk tx_operation_sb;
    size_t tx_operation_allocated_count;
    size_t tx_operation_deferred_count;
    int tx_operation_had_last_deferred;
    struct infs_deferred_range tx_operation_last_deferred;
    uint64_t tx_operation_pending_bytes;
    uint64_t tx_operation_data_cursor;
    uint64_t tx_operation_metadata_cursor;

    int deferred_publish;
    uint64_t deferred_publish_threshold_bytes;
    uint64_t tx_pending_bytes;

    uint64_t data_allocation_cursor;
    uint64_t metadata_allocation_cursor;
    struct infs_free_extent *free_extents;
    size_t free_extent_count;
    size_t free_extent_capacity;
    int free_extent_index_valid;
    struct infs_object_cache_entry *object_cache;
    size_t object_cache_slots;
    int object_cache_complete;
    struct infs_directory_cache_entry *directory_cache;
    size_t directory_cache_slots;
    struct infs_directory_cache_state *directory_cache_states;
    size_t directory_cache_state_slots;

    uint8_t checksum_cursor_owner_id[16];
    uint8_t checksum_cursor_object_id[16];
    uint64_t checksum_cursor_start;
    int checksum_cursor_valid;
    uint64_t checksum_cursor_hits;
    uint64_t checksum_chain_steps;

    void *snapshot_validation_context;
    void *snapshot_scrub_context;

    int snapshot_view;
    uint64_t snapshot_bitmap_start_block;
    uint64_t snapshot_bitmap_block_count;
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
    uint64_t scrub_generation;
    uint64_t snapshots_checked;
};

enum infs_scrub_phase {
    INFS_SCRUB_PHASE_METADATA = 1,
    INFS_SCRUB_PHASE_DATA = 2,
    INFS_SCRUB_PHASE_SNAPSHOTS = 3,
    INFS_SCRUB_PHASE_COMPLETE = 4
};

/* Metadata scrub sub-stages are deliberately explicit. A full metadata pass
 * performs several independently expensive graph walks; exposing the active
 * sub-stage plus a real completed/total counter makes a slow pass diagnosable
 * instead of presenting a motionless "metadata 0/0" line. */
enum infs_scrub_metadata_stage {
    INFS_SCRUB_METADATA_NONE = 0,
    INFS_SCRUB_METADATA_INDEX_OBJECTS = 1,
    INFS_SCRUB_METADATA_OWNERSHIP_OBJECTS = 2,
    INFS_SCRUB_METADATA_SNAPSHOT_BITMAP = 3,
    INFS_SCRUB_METADATA_SNAPSHOT_UNION = 4,
    INFS_SCRUB_METADATA_OWNERSHIP_BITMAP = 5,
    INFS_SCRUB_METADATA_INTEGRITY_OBJECTS = 6,
    INFS_SCRUB_METADATA_NAMESPACE_DIRECTORIES = 7,
    INFS_SCRUB_METADATA_NAMESPACE_LINKS = 8,
    INFS_SCRUB_METADATA_NAMESPACE_REACHABILITY = 9,
    INFS_SCRUB_METADATA_CHECKSUM_FILES = 10,
    INFS_SCRUB_METADATA_SNAPSHOT_GENERATIONS = 11,
    INFS_SCRUB_METADATA_OWNERSHIP_DATA_BLOCKS = 12
};

struct infs_scrub_progress {
    uint32_t phase;
    uint32_t metadata_stage;
    uint64_t metadata_generation;
    uint64_t metadata_items_checked;
    uint64_t metadata_items_total;
    uint64_t files_checked;
    uint64_t data_blocks_checked;
    uint64_t snapshots_checked;
    uint64_t checksum_errors;
    uint64_t metadata_errors;
};

typedef void (*infs_scrub_progress_fn)(
    const struct infs_scrub_progress *progress, void *context);

struct infs_compression_metrics {
    uint64_t generation;
    uint64_t files_scanned;
    uint64_t snapshots_scanned;
    uint64_t referenced_logical_bytes;
    uint64_t compressed_referenced_logical_bytes;
    uint64_t unique_compressed_logical_bytes;
    uint64_t unique_compressed_physical_bytes;
    uint64_t compression_saved_bytes;
    uint64_t unique_compressed_streams;
};

struct infs_dir_item {
    char name[INFS_NAME_MAX + 1u];
    uint8_t object_id[16];
    uint16_t type;
};

struct infs_snapshot_info {
    char name[INFS_SNAPSHOT_NAME_MAX + 1u];
    uint64_t generation;
    struct infs_timestamp created_time;
};

infs_status infs_volume_open_storage(struct infs_volume *vol,
                                     struct infs_storage *storage,
                                     int writable);
void infs_volume_close(struct infs_volume *vol);
infs_status infs_volume_sync(struct infs_volume *vol);
infs_status infs_volume_resize(struct infs_volume *vol,
                               uint64_t new_size_bytes);
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
infs_status infs_link_file(struct infs_volume *vol, const char *existing_path,
                           const char *new_path);
infs_status infs_unlink(struct infs_volume *vol, const char *path);
infs_status infs_rmdir(struct infs_volume *vol, const char *path);
infs_status infs_rename(struct infs_volume *vol, const char *oldpath,
                        const char *newpath);

int64_t infs_read_file(struct infs_volume *vol, const char *path, void *buf,
                       size_t size, uint64_t offset);
int64_t infs_write_file(struct infs_volume *vol, const char *path,
                        const void *buf, size_t size, uint64_t offset);
int64_t infs_write_file_buffered(struct infs_volume *vol, const char *path,
                                 const void *buf, size_t size,
                                 uint64_t offset);
infs_status infs_truncate_file(struct infs_volume *vol, const char *path,
                               uint64_t size);
infs_status infs_punch_hole(struct infs_volume *vol, const char *path,
                            uint64_t offset, uint64_t length);
infs_status infs_reflink_file(struct infs_volume *vol, const char *source_path,
                              const char *destination_path);

infs_status infs_snapshot_create(struct infs_volume *vol, const char *name);
infs_status infs_snapshot_delete(struct infs_volume *vol, const char *name);
infs_status infs_snapshot_list(struct infs_volume *vol,
                               struct infs_snapshot_info **snapshots,
                               size_t *count);
void infs_free_snapshot_infos(struct infs_snapshot_info *snapshots);
infs_status infs_snapshot_lookup_path(struct infs_volume *vol,
                                      const char *snapshot, const char *path,
                                      struct infs_lookup *out);
infs_status infs_snapshot_get_attributes(struct infs_volume *vol,
                                          const char *snapshot,
                                          const char *path,
                                          struct infs_attributes *attributes);
infs_status infs_snapshot_list_dir(struct infs_volume *vol,
                                   const char *snapshot, const char *path,
                                   struct infs_dir_item **items,
                                   size_t *count);
int64_t infs_snapshot_read_file(struct infs_volume *vol,
                                const char *snapshot, const char *path,
                                void *buf, size_t size, uint64_t offset);
infs_status infs_snapshot_read_symlink(struct infs_volume *vol,
                                       const char *snapshot,
                                       const char *path, char *target,
                                       size_t capacity, size_t *length_out);

infs_status infs_set_posix_compat(struct infs_volume *vol, const char *path,
                                  uint32_t mask, uint32_t permissions,
                                  uint32_t uid, uint32_t gid);
infs_status infs_set_times(struct infs_volume *vol, const char *path,
                           const struct infs_time_update *update);
infs_status infs_set_portable_flags(struct infs_volume *vol, const char *path,
                                    uint64_t portable_flags);

infs_status infs_scrub(struct infs_volume *vol,
                       struct infs_scrub_report *report);
/* Run the same authoritative scrub while reporting real work completed.
 * Metadata progress includes the active graph-validation sub-stage, retained
 * generation, and an actual completed/total work counter. Progress callbacks
 * are advisory and never alter on-disk state. */
infs_status infs_scrub_with_progress(
    struct infs_volume *vol, struct infs_scrub_report *report,
    infs_scrub_progress_fn progress, void *context);
infs_status infs_compression_metrics(
    struct infs_volume *vol, struct infs_compression_metrics *metrics);
infs_status infs_snapshot_scrub(struct infs_volume *vol, const char *snapshot,
                                struct infs_scrub_report *report);
infs_status infs_scrub_online(struct infs_volume *vol,
                              struct infs_scrub_report *report);

#endif
