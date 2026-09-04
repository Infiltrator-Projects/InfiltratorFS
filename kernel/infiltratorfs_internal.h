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

#define INFILFS_RW_FREE_RANGES_INITIAL 32u
#define INFILFS_RW_ALLOC_RANGES_INITIAL 64u

/* Private transaction/allocation state shared by compiled native components. */
struct infilfs_rw_free_range {
    u64 start;
    u64 count;
};

struct infilfs_rw_tx {
    struct super_block *sb;
    struct infilfs_sb_info *sbi;
    struct infilfs_superblock_disk next_sb;
    u8 *bitmap;
    size_t bitmap_bytes;
    u64 generation;
    u64 free_blocks;
    struct infilfs_rw_free_range *deferred;
    size_t deferred_count;
    size_t deferred_capacity;
    struct infilfs_rw_free_range *allocated;
    size_t allocated_count;
    size_t allocated_capacity;
    struct infilfs_rw_free_range *free_extents;
    size_t free_extent_count;
    size_t free_extent_capacity;
    bool free_extent_index_valid;
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

static inline struct infilfs_inode_info *INFILFS_I(struct inode *inode)
{
    return inode->i_private;
}

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

/* Bounded cycle/alias detector shared by metadata-tree walkers. */
struct infilfs_visit_set {
    u64 *slots;
    size_t capacity;
    size_t count;
};

struct infilfs_native_checksum_payload_disk {
    u8 owner_object_id[16];
    u8 next_object_id[16];
    __le64 start_logical_block;
    __le32 checksum_count;
    __le32 reserved;
} __packed;

#define INFILFS_NATIVE_CHECKSUMS_PER_OBJECT \
    ((INFILFS_DISK_BLOCK_SIZE - sizeof(struct infilfs_object_header_disk) - \
      sizeof(struct infilfs_native_checksum_payload_disk)) / \
     sizeof(struct infilfs_data_checksum_disk))

struct infilfs_native_checksum_cache_entry {
    struct super_block *sb;
    u8 owner_id[16];
    u8 object_id[16];
    u64 object_block;
    u64 start_logical;
    bool valid;
};

#define INFILFS_NATIVE_READ_INTEGRITY_PARITY 1
struct infilfs_native_read_checksum_cursor {
    u8 object_id[16];
    u64 object_block;
    u64 start_logical;
    bool valid;
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

/* Services and entry points shared with the compiled object-index tree. */
extern const u8 infilfs_index_page_magic[8];
extern const u8 infilfs_index_branch_page_magic[8];
bool infilfs_block_allocated(struct super_block *sb, u64 block);
int infilfs_read_allocated_block(struct super_block *sb, u64 block, void *out);
bool infilfs_metadata_page_valid(
    struct super_block *sb, const u8 *block, const u8 magic[8],
    const u8 owner_id[16]);
int infilfs_visit_claim(struct infilfs_visit_set *set, u64 block);
void infilfs_visit_destroy(struct infilfs_visit_set *set);
bool infilfs_index_tree_branch_valid(
    struct super_block *sb, const u8 block[INFILFS_DISK_BLOCK_SIZE],
    const u8 owner_id[16], const __le64 **children_out, u32 *count_out);
int infilfs_index_tree_lookup_head(
    struct super_block *sb, const u8 head[INFILFS_DISK_BLOCK_SIZE],
    const u8 object_id[16], u64 *object_block_out, u16 *type_out);
int infilfs_index_tree_snapshot(
    struct super_block *sb, const u8 head[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_index_entry_disk **entries_out, u32 *count_out);

/* Services shared with the independently compiled resize component. */
bool infilfs_rw_bitmap_get(const u8 *bitmap, u64 block);
void infilfs_rw_bitmap_set(u8 *bitmap, u64 block, bool allocated);
int infilfs_rw_snapshot_count(struct super_block *sb, u32 *count_out);
int infilfs_rw_encode_superblock(
    const struct infilfs_superblock_disk *disk,
    u8 block[INFILFS_DISK_BLOCK_SIZE]);
int infilfs_rw_write_block(struct super_block *sb, u64 block, const void *data);
int infilfs_rw_allocation_write_page(
    struct super_block *sb, u64 physical, const u8 magic[8],
    u64 generation, u64 logical, u32 level, u32 entries,
    const void *payload, u32 bytes);
void infilfs_parallel_shard_bounds(
    const struct infilfs_sb_info *sbi, u32 shard, u64 *start, u64 *end);
int infilfs_native_pending_flush_sb(struct super_block *sb);
int infilfs_native_resize_volume(
    struct super_block *sb, struct infilfs_resize_request *request);

/* Services shared with the compiled volatile parallel allocator. */
const char *infilfs_media_profile_name(enum infilfs_media_profile profile);
void infilfs_rw_free_extent_index_invalidate(struct infilfs_rw_tx *tx);
int infilfs_rw_free_extent_index_remove(
    struct infilfs_rw_tx *tx, u64 start, u64 count);
u64 infilfs_native_metadata_reserve_blocks(const struct infilfs_sb_info *sbi);
u64 infilfs_native_visible_free_blocks(struct super_block *sb);
int infilfs_parallel_allocator_mount_init(struct super_block *sb);
void infilfs_parallel_allocator_mount_destroy(struct super_block *sb);
int infilfs_parallel_tx_claim(
    struct infilfs_rw_tx *tx, u64 start, u64 count, bool consume_reservation);
bool infilfs_parallel_range_reserved(
    const struct infilfs_sb_info *sbi, u64 start, u64 count);
u64 infilfs_parallel_object_preferred(
    const struct infilfs_sb_info *sbi, const u8 object_id[16]);
void infilfs_parallel_note_workload(
    struct infilfs_sb_info *sbi, enum infilfs_data_workload workload);
int infilfs_parallel_reserve_data(
    struct super_block *sb, u64 count, u64 preferred,
    struct infilfs_parallel_reservation *reservation);
void infilfs_parallel_release_reservation(
    struct super_block *sb, struct infilfs_parallel_reservation *reservation);
int infilfs_parallel_consume_reservation(
    struct infilfs_rw_tx *tx, struct infilfs_parallel_reservation *reservation,
    u64 count, u64 *start_out);

/* Transaction services shared with the compiled allocation publisher. */
u64 infilfs_rw_crc64_zeroed(
    const u8 *data, size_t length, size_t zero_offset, size_t zero_length);
int infilfs_rw_tx_alloc(struct infilfs_rw_tx *tx, u64 count, u64 *start_out);
int infilfs_rw_tx_defer_free(struct infilfs_rw_tx *tx, u64 start, u64 count);
int infilfs_rw_tx_apply_deferred(struct infilfs_rw_tx *tx);
int infilfs_rw_allocation_map_publish(
    struct infilfs_rw_tx *tx, struct infilfs_allocation_layout *next_layout);

/* Services shared with the compiled verified-read cursor/cache layer. */
extern const u8 infilfs_extent_page_magic[8];
u32 infilfs_extent_kind(u32 flags);
bool infilfs_extent_is_compressed(u32 flags);
bool infilfs_extent_flags_valid(u32 logical_blocks, u64 physical, u32 flags);
int infilfs_read_compressed_extent(
    struct inode *inode, u64 physical, u32 extent_blocks, u32 flags,
    u8 *plain, size_t plain_capacity);
int infilfs_map_file_block_detail(
    struct inode *inode, const u8 *object, u64 logical,
    u64 *physical_out, u32 *flags_out,
    u64 *extent_logical_out, u32 *extent_blocks_out);
int infilfs_read_object(
    struct super_block *sb, u64 object_block, u16 expected_type,
    const u8 *expected_id, u8 *out);
bool infilfs_native_checksum_cache_lookup(
    struct super_block *sb, const u8 owner_id[16],
    struct infilfs_native_checksum_cache_entry *out);
void infilfs_native_checksum_cache_store(
    struct super_block *sb, const u8 owner_id[16], const u8 object_id[16],
    u64 object_block, u64 start_logical);
int infilfs_native_read_expected_digest(
    struct super_block *sb, const u8 owner_id[16], const u8 head_id[16],
    u64 logical, struct infilfs_native_read_checksum_cursor *cursor,
    u8 checksum_object[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_data_checksum_disk *digest_out);
void infilfs_native_block_digest(
    const u8 data[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_data_checksum_disk *digest);
int infilfs_rw_inline_digest(const u8 *data, size_t size, u8 out[32]);
ssize_t infilfs_native_read_iter_cached(
    struct inode *inode, loff_t *position, struct iov_iter *to);
ssize_t infilfs_file_read_iter_cached(struct kiocb *iocb, struct iov_iter *to);
ssize_t infilfs_native_extent_write_iter(
    struct inode *inode, loff_t *position, struct iov_iter *from,
    size_t requested);
extern const struct address_space_operations infilfs_aops;

#endif /* INFILTRATORFS_INTERNAL_H */
