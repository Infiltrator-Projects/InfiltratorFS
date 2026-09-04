// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATORFS_INTERNAL_H
#define INFILTRATORFS_INTERNAL_H

#include <linux/atomic.h>
#include <linux/blkdev.h>
#include <linux/bitops.h>
#include <linux/buffer_head.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/delay.h>
#include <linux/dirent.h>
#include <linux/falloc.h>
#include <linux/fiemap.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/lz4.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pagevec.h>
#include <linux/pagemap.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/uidgid.h>
#include <linux/uio.h>
#include <linux/user_namespace.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/writeback.h>
#include <linux/xattr.h>

#include "infiltratorfs_format.h"
#include "infiltratorfs_ioctl.h"
#include "iac1.h"

#define INFILTRATORFS_NAME "infiltratorfs"
#define INFILTRATORFS_MAGIC 0x494e4653u
#define INFILFS_ALLOCATION_RESERVATION_SHARDS 64u
#define INFILFS_LINUX_META_DIRECTORY ".infilfs-posix-meta"

struct infilfs_parallel_reservation {
    u64 start;
    u64 count;
    u32 shard;
    bool active;
};

enum infilfs_data_workload {
    INFILFS_DATA_WORKLOAD_SEQUENTIAL = 0,
    INFILFS_DATA_WORKLOAD_RANDOM,
    INFILFS_DATA_WORKLOAD_SPARSE,
};

enum infilfs_media_profile {
    INFILFS_MEDIA_BALANCED = 0,
    INFILFS_MEDIA_ROTATIONAL,
    INFILFS_MEDIA_NONROTATIONAL,
};

enum infilfs_media_override {
    INFILFS_MEDIA_OVERRIDE_AUTO = 0,
    INFILFS_MEDIA_OVERRIDE_BALANCED,
    INFILFS_MEDIA_OVERRIDE_ROTATIONAL,
    INFILFS_MEDIA_OVERRIDE_NONROTATIONAL,
};

struct infilfs_fs_context {
    enum infilfs_media_override media_override;
};

struct infilfs_quota_rule;
struct infilfs_project_root;

struct infilfs_sb_info {
    struct infilfs_superblock_disk disk;
    u64 device_blocks;
    struct mutex write_lock;
    struct mutex linux_meta_lock;
    struct mutex resize_lock;
    bool resize_active;
    struct mutex quota_lock;
    struct infilfs_quota_rule *quota_rules;
    size_t quota_rule_count;
    struct infilfs_project_root *project_roots;
    size_t project_root_count;
    rwlock_t bitmap_lock;
    u8 *bitmap;
    size_t bitmap_bytes;
    u64 *allocation_leaf_blocks;
    u64 *allocation_branch_blocks;
    size_t allocation_leaf_count;
    size_t allocation_branch_count;
    size_t allocation_level1_count;
    size_t allocation_level2_count;
    const u8 *visible_bitmap;
    size_t visible_bitmap_bytes;
    const u8 *validation_bitmap;
    size_t validation_bitmap_bytes;
    u8 *snapshot_bitmap;
    u64 data_alloc_hint;
    u64 metadata_alloc_hint;
    spinlock_t allocation_reservation_locks[
        INFILFS_ALLOCATION_RESERVATION_SHARDS];
    unsigned long *allocation_reservations;
    size_t allocation_reservation_bytes;
    u64 allocation_reservation_hints[
        INFILFS_ALLOCATION_RESERVATION_SHARDS];
    atomic64_t allocation_reservation_steer;
    atomic64_t allocation_reserved_blocks;
    atomic64_t allocation_active_reservations;
    atomic64_t allocation_peak_active_reservations;
    atomic64_t allocation_reservation_successes;
    atomic64_t allocation_reservation_conflicts;
    atomic64_t allocation_workload_sequential;
    atomic64_t allocation_workload_random;
    atomic64_t allocation_workload_sparse;
    atomic64_t allocation_locality_scored;
    atomic64_t allocation_best_fit;
    atomic64_t allocation_media_rotational_scored;
    atomic64_t allocation_media_nonrotational_scored;
    atomic64_t allocation_media_balanced_scored;
    enum infilfs_media_profile media_profile;
    bool media_profile_overridden;
    bool rw_enabled;
    bool write_poisoned;
    bool checkpoint_repair_needed;
};

struct infilfs_inode_info {
    u64 object_block;
    u64 data_allocation_hint;
    u64 portable_flags;
    u16 object_type;
    u8 object_id[16];
    char *symlink_target;
};

struct infilfs_dir_lookup {
    const char *name;
    size_t name_len;
    u8 object_id[16];
    u16 object_type;
    bool found;
};

struct infilfs_dir_emit_state {
    struct dir_context *ctx;
    struct inode *dir;
    bool has_linux_meta;
    bool hide_linux_meta;
    u64 index;
};

struct infilfs_dir_snapshot_entry {
    struct list_head link;
    u8 object_id[16];
    u16 object_type;
    u16 name_len;
    u8 name[];
};

struct infilfs_dir_snapshot {
    struct list_head entries;
};

struct infilfs_allocation_layout {
    u64 *leaf_blocks;
    u64 *branch_blocks;
    size_t leaf_count;
    size_t branch_count;
    size_t level1_count;
    size_t level2_count;
};

static inline struct infilfs_sb_info *INFILFS_SB(struct super_block *sb)
{
    return sb->s_fs_info;
}

static inline u64 infilfs_volume_blocks(const struct infilfs_sb_info *sbi)
{
    return sbi ? le64_to_cpu(sbi->disk.total_blocks) : 0;
}

bool infilfs_crc64_block_valid(
    const u8 block[INFILFS_DISK_BLOCK_SIZE],
    size_t checksum_offset, size_t checksum_size);
int infilfs_read_block(struct super_block *sb, u64 block, void *out);

void infilfs_allocation_layout_destroy(struct infilfs_allocation_layout *layout);
void infilfs_allocation_cache_destroy(struct infilfs_sb_info *sbi);
void infilfs_allocation_cache_replace(
    struct infilfs_sb_info *sbi, struct infilfs_allocation_layout *layout);
int infilfs_allocation_cache_view(
    struct infilfs_sb_info *sbi,
    const struct infilfs_superblock_disk *disk,
    struct infilfs_allocation_layout *layout);
int infilfs_allocation_counts(
    u64 total, size_t *leaves_out, size_t *level1_out,
    size_t *level2_out, size_t *branches_out);
int infilfs_allocation_runtime_bytes(u64 total, size_t *bytes_out);
int infilfs_allocation_map_load(
    struct super_block *sb, const struct infilfs_superblock_disk *disk,
    u8 **bitmap_out, size_t *bitmap_bytes_out,
    struct infilfs_allocation_layout *layout_out);

#endif /* INFILTRATORFS_INTERNAL_H */
