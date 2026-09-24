// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATORFS_INTERNAL_H
#define INFILTRATORFS_INTERNAL_H

#include <linux/atomic.h>
#include <linux/blkdev.h>
#include <linux/bitops.h>
#include <linux/buffer_head.h>
#include <linux/capability.h>
#include <linux/completion.h>
#include <linux/cpumask.h>
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
#include <linux/list.h>
#include <linux/lz4.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pagevec.h>
#include <linux/pagemap.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <linux/random.h>
#include <linux/rwsem.h>
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
#include <linux/wait.h>
#include <linux/vmalloc.h>
#include <linux/writeback.h>
#include <linux/workqueue.h>
#include <linux/xattr.h>

#include "infiltratorfs_format.h"
#include "infiltratorfs_ioctl.h"
#include "iac1.h"

#define INFILTRATORFS_NAME "infiltratorfs"
#define INFILTRATORFS_MAGIC 0x494e4653u
#define INFILFS_ALLOCATION_RESERVATION_SHARDS 64u
#define INFILFS_NATIVE_WRITEBACK_BATCH_BYTES (1024u * 1024u)

/*
 * Native CPU parallelism is module-wide, not per mount.  All mounted volumes
 * share one execution budget so multiple InfiltratorFS mounts cannot each
 * consume N-1 CPUs independently.  The implementation in infiltratorfs_cpu.c
 * derives the budget exactly as max(1, online logical CPUs - 1).
 */
int infilfs_cpu_pool_init(void);
void infilfs_cpu_pool_exit(void);
unsigned int infilfs_cpu_budget(void);
bool infilfs_queue_cpu_work(struct work_struct *work);
void infilfs_mod_delayed_cpu_work(struct delayed_work *work,
                                  unsigned long delay);
void infilfs_cpu_work_enter(void);
void infilfs_cpu_work_exit(void);
bool infilfs_removable_name_valid_v1(const unsigned char *name, size_t length);
bool infilfs_casefold_names_enabled(const struct super_block *sb);
bool infilfs_name_equal(const struct super_block *sb,
                        const u8 *left, size_t left_length,
                        const u8 *right, size_t right_length);
u32 infilfs_name_hash(const struct super_block *sb,
                      const u8 *name, size_t length);
extern const struct dentry_operations infilfs_casefold_dentry_ops;
int infilfs_name_validate(struct super_block *sb, const struct qstr *name);
bool infilfs_name_is_reserved_linux_meta(const struct qstr *name);

bool infilfs_security_reserved_principal_id(const u8 id[16]);
bool infilfs_security_sid_valid(const u8 *sid, u16 size);
bool infilfs_security_ace_valid(
    const struct infilfs_security_ace_disk *ace);
bool infilfs_security_binding_index_valid(
    const struct infilfs_object_header_disk *header,
    const struct infilfs_security_binding_index_payload_disk *payload);
bool infilfs_security_descriptor_valid(
    struct super_block *sb, const struct infilfs_object_header_disk *header,
    u32 payload_size);

static inline size_t infilfs_native_writeback_batch_bytes(void)
{
    const size_t cluster_bytes =
        (size_t)INFILFS_COMPRESSION_CLUSTER_BLOCKS * INFILFS_DISK_BLOCK_SIZE;
    unsigned int budget = infilfs_cpu_budget();
    size_t scaled;

    if (budget > SIZE_MAX / cluster_bytes)
        scaled = SIZE_MAX & ~((size_t)INFILFS_DISK_BLOCK_SIZE - 1u);
    else
        scaled = cluster_bytes * budget;
    return max_t(size_t, INFILFS_NATIVE_WRITEBACK_BATCH_BYTES, scaled);
}
#define INFILFS_LINUX_META_DIRECTORY ".infilfs-posix-meta"

#define INFILFS_LINUX_META_MAGIC "INPSXM01"
#define INFILFS_LINUX_META_VERSION 1u
#define INFILFS_LINUX_META_MAX (1024u * 1024u)

struct infilfs_linux_meta_header {
    u8 magic[8];
    __le32 version;
    __le32 special_mode;
    __le64 special_rdev;
    __le32 xattr_bytes;
    __le32 reserved;
} __packed;

struct infilfs_linux_xattr_record {
    __le16 name_length;
    __le16 reserved;
    __le32 value_length;
} __packed;

static_assert(sizeof(struct infilfs_linux_meta_header) == 32);
static_assert(sizeof(struct infilfs_linux_xattr_record) == 8);


struct infilfs_parallel_reservation {
    u64 start;
    u64 count;
    u32 shard;
    bool active;
};


static inline bool infilfs_timestamp_valid(
    const struct infilfs_timestamp_disk *value)
{
    return value && le32_to_cpu(value->nanoseconds) < 1000000000u &&
           le32_to_cpu(value->reserved) == 0;
}

static inline void infilfs_timestamp_encode(
    struct infilfs_timestamp_disk *out, struct timespec64 value)
{
    out->seconds = cpu_to_le64((u64)value.tv_sec);
    out->nanoseconds = cpu_to_le32((u32)value.tv_nsec);
    out->reserved = 0;
}

static inline void infilfs_timestamp_encode_ns(
    struct infilfs_timestamp_disk *out, s64 ns)
{
    infilfs_timestamp_encode(out, ns_to_timespec64(ns));
}

static inline struct timespec64 infilfs_timestamp_decode(
    const struct infilfs_timestamp_disk *value)
{
    struct timespec64 out = {
        .tv_sec = (s64)le64_to_cpu(value->seconds),
        .tv_nsec = (long)le32_to_cpu(value->nanoseconds),
    };
    return out;
}

static inline bool infilfs_common_attributes_valid(
    const struct infilfs_attributes_disk *attributes)
{
    return attributes &&
        !(le64_to_cpu(attributes->portable_flags) & ~INFILFS_KNOWN_ATTR_FLAGS) &&
        infilfs_timestamp_valid(&attributes->birth_time) &&
        infilfs_timestamp_valid(&attributes->access_time) &&
        infilfs_timestamp_valid(&attributes->modification_time) &&
        infilfs_timestamp_valid(&attributes->change_time);
}

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

struct infilfs_rw_sha256_ctx {
    u32 state[8];
    u64 total;
    u8 block[64];
    u32 used;
};

#define INFILFS_NATIVE_WRITER_TAIL_SLOTS 64u
struct infilfs_native_undo {
    u64 block;
    u8 data[INFILFS_DISK_BLOCK_SIZE];
};

struct infilfs_native_writer_tail {
    u8 owner_id[16];
    u8 object_id[16];
    u64 object_block;
    u64 start_logical;
    bool valid;
};

struct infilfs_native_index_locator {
    u8 object_id[16];
    u32 page_index;
    u32 entry_index;
    bool valid;
};

struct infilfs_native_directory_locator {
    size_t name_offset;
    u32 hash;
    u16 name_len;
    bool valid;
};

struct infilfs_native_shared_range {
    u64 start;
    u64 end;
    u32 refs;
};

struct infilfs_native_pending {
    struct list_head node;
    struct super_block *sb;
    struct infilfs_rw_tx tx;
    struct delayed_work idle_work;
    size_t operation_allocated_count;
    struct infilfs_superblock_disk operation_next_sb;
    u64 operation_free_blocks;
    size_t operation_deferred_count;
    struct infilfs_rw_free_range operation_last_deferred;
    bool operation_had_last;
    struct infilfs_native_undo *undo;
    unsigned int undo_count;
    unsigned int undo_capacity;
    /*
     * Exact set of blocks allocated by the current unpublished transaction.
     * sbi->bitmap and tx.bitmap intentionally share one working allocation
     * image, so comparing those bitmaps cannot identify transaction-private
     * blocks.  This volatile set allows repeated metadata updates to reuse
     * private CoW blocks safely with the per-operation undo journal.
     */
    struct infilfs_visit_set private_blocks;
    struct infilfs_native_writer_tail
        writer_tail[INFILFS_NATIVE_WRITER_TAIL_SLOTS];
    struct infilfs_native_index_locator *index_locators;
    u32 index_locator_capacity;
    u32 index_locator_count;
    bool index_locator_valid;
    struct infilfs_native_directory_locator *directory_locators;
    u8 *directory_locator_names;
    size_t directory_locator_names_bytes;
    size_t directory_locator_names_capacity;
    u8 directory_locator_owner_id[16];
    u32 directory_locator_capacity;
    u32 directory_locator_count;
    bool directory_locator_valid;
    struct infilfs_native_shared_range *shared_ranges;
    size_t shared_range_count;
    bool shared_range_index_valid;
    u64 pending_bytes;
    u64 pending_physical_bytes;
    u64 publish_threshold;
    bool active;
    bool commit_failed;
};

/* Rebuildable native lookup accelerators; persistent state never depends on them. */
u32 infilfs_native_id_hash(const u8 id[16]);
void infilfs_native_writer_tail_invalidate(struct infilfs_native_pending *pending);
bool infilfs_native_writer_tail_lookup(
    struct infilfs_native_pending *pending, const u8 owner_id[16],
    struct infilfs_native_writer_tail *out);
void infilfs_native_writer_tail_store(
    struct infilfs_native_pending *pending, const u8 owner_id[16],
    const u8 object_id[16], u64 object_block, u64 start_logical);
void infilfs_native_index_locator_invalidate(
    struct infilfs_native_pending *pending);
int infilfs_native_index_locator_ensure(
    struct infilfs_native_pending *pending, u32 wanted);
int infilfs_native_index_locator_insert(
    struct infilfs_native_pending *pending, const u8 object_id[16],
    u32 page_index, u32 entry_index);
bool infilfs_native_index_locator_lookup(
    struct infilfs_native_pending *pending, const u8 object_id[16],
    u32 *page_index, u32 *entry_index);
int infilfs_native_index_locator_build(
    struct infilfs_native_pending *pending,
    const u8 head[INFILFS_DISK_BLOCK_SIZE]);
void infilfs_native_directory_locator_invalidate(
    struct infilfs_native_pending *pending);
int infilfs_native_directory_locator_ensure(
    struct infilfs_native_pending *pending, u32 wanted);
bool infilfs_native_directory_locator_matches(
    struct infilfs_native_pending *pending, const u8 owner_id[16], u32 count);
bool infilfs_native_directory_locator_lookup(
    struct infilfs_native_pending *pending, const u8 owner_id[16],
    const char *name, size_t name_len);
int infilfs_native_directory_locator_insert(
    struct infilfs_native_pending *pending, const u8 owner_id[16],
    const char *name, size_t name_len);

int infilfs_native_collect_extents(
    struct infilfs_native_pending *pending, struct inode *inode,
    u8 object[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_extent_disk **extents_out, u32 *count_out,
    u64 *old_blocks_out, bool *was_inline_out);
int infilfs_ns_index_snapshot(
    struct super_block *sb, struct infilfs_index_entry_disk **entries_out,
    u32 *count_out);
int infilfs_shared_ownership_index_build(
    struct infilfs_native_pending *pending);
int infilfs_shared_ownership_prepare_index(
    struct infilfs_native_pending *pending);
bool infilfs_shared_ownership_maybe_shared(
    const struct infilfs_native_pending *pending, u64 start, u64 end);
int infilfs_shared_ownership_other_reference_cover(
    struct super_block *sb, const u8 owner_id[16], u64 cursor, u64 end,
    u64 *cover_end, u64 *next_start);
int infilfs_shared_ownership_add_owner(
    struct infilfs_native_pending *pending,
    const struct infilfs_extent_disk *extents, u32 extent_count);
int infilfs_shared_ownership_drop_owner(
    struct infilfs_native_pending *pending, struct inode *inode);


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

enum infilfs_compression_mode {
    INFILFS_COMPRESSION_MODE_AUTO = 0,
    INFILFS_COMPRESSION_MODE_OFF,
};

struct infilfs_fs_context {
    enum infilfs_media_override media_override;
    enum infilfs_compression_mode compression_mode;
    bool media_specified;
    bool compression_specified;
};

struct infilfs_quota_subject {
    u32 type;
    u32 id;
};

struct infilfs_quota_reservation {
    struct super_block *sb;
    struct infilfs_quota_subject subject[3];
    u32 subject_count;
    u64 bytes;
    u64 objects;
    bool active;
};

struct infilfs_quota_rule;
struct infilfs_project_root;

#define INFILFS_LINUX_META_CACHE_BUCKETS 4096u

struct infilfs_linux_meta_cache_entry {
    struct hlist_node node;
    u8 target_object_id[16];
    u8 sidecar_object_id[16];
    u64 sidecar_object_block;
};

struct infilfs_sb_info {
    struct infilfs_superblock_disk disk;
    u64 device_blocks;
    struct rw_semaphore write_lock;
    /*
     * Serializes construction of the volatile shared-range ownership index.
     * Builders run under write_lock read-side topology protection, never the
     * writer side, so a million-file rebuild cannot block unrelated readers.
     */
    struct mutex shared_range_build_lock;
    spinlock_t pagecache_accounting_lock;
    atomic64_t pagecache_pending_blocks;
    struct mutex linux_meta_lock;
    struct hlist_head *linux_meta_cache;
    bool linux_meta_cache_valid;
    bool linux_meta_dir_checked;
    bool linux_meta_dir_cached;
    u8 linux_meta_dir_object_id[16];
    u64 linux_meta_dir_object_block;
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

    /*
     * Rebuildable mount-owned index of maximal free runs. Transactions borrow
     * this accelerator while write_lock serializes bitmap mutation and return
     * it after publication. The allocation bitmap remains authoritative.
     */
    struct infilfs_rw_free_range *free_extents;
    size_t free_extent_count;
    size_t free_extent_capacity;
    bool free_extent_index_valid;

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
    atomic64_t prepared_append_attempts;
    atomic64_t prepared_append_active;
    atomic64_t prepared_append_peak_active;
    atomic64_t prepared_append_successes;
    atomic64_t prepared_append_bytes;
    atomic64_t prepared_paged_append_successes;
    enum infilfs_media_profile media_profile;
    bool media_profile_overridden;
    bool compression_enabled;
    bool rw_enabled;
    bool write_poisoned;
    bool checkpoint_repair_needed;

    /*
     * Crash-orphan discovery can be proportional to the number of files.
     * Keep it off the mount(2) critical path without globally stalling live
     * namespace mutation. orphan_recovery_generation is the committed mount
     * generation: only zero-link objects at or below that generation can be
     * leftovers from the previous boot. New unlink activity necessarily
     * publishes a newer object generation and is therefore never reclaimed by
     * the background recovery pass.
     */
    struct super_block *orphan_recovery_sb;
    struct delayed_work orphan_recovery_work;
    u64 orphan_recovery_generation;
};

struct infilfs_inode_info {
    u64 object_block;
    u64 persisted_size;
    u64 data_allocation_hint;
    u64 portable_flags;
    struct timespec64 birth_time;
    u16 object_type;
    u8 object_id[16];
    char *symlink_target;
};

static inline struct infilfs_inode_info *INFILFS_I(struct inode *inode)
{
    return inode->i_private;
}

struct infilfs_dir_lookup {
    struct super_block *sb;
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

struct infilfs_native_index_change {
    u8 object_id[16];
    u64 object_block;
    u16 object_type;
    bool add;
    bool remove;
    bool found;
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
int infilfs_crypto_sha256(const u8 *data, size_t len, u8 out[32]);
int infilfs_crypto_sha256_zeropad(const u8 *data, size_t len,
                                  size_t padded_len, u8 out[32]);
void infilfs_crypto_exit(void);
bool infilfs_extension_object_valid(
    const struct infilfs_object_header_disk *header,
    u16 version, u32 payload_size);

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
int infilfs_read_allocated_blocks(
    struct super_block *sb, u64 start, u32 count, void *out);
bool infilfs_metadata_page_valid(
    struct super_block *sb, const u8 *block, const u8 magic[8],
    const u8 owner_id[16]);
int infilfs_visit_claim(struct infilfs_visit_set *set, u64 block);
bool infilfs_visit_contains(const struct infilfs_visit_set *set, u64 block);
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
int infilfs_rw_free_extent_index_rebuild_mount(struct super_block *sb);
u64 infilfs_native_metadata_reserve_blocks(const struct infilfs_sb_info *sbi);
u64 infilfs_native_visible_free_blocks(struct super_block *sb);
int infilfs_parallel_allocator_mount_init(struct super_block *sb);
int infilfs_parallel_allocator_enable(struct super_block *sb);
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
extern const u8 infilfs_extent_index_page_magic[8];
u32 infilfs_extent_pointer_tree_levels(u32 page_count);
bool infilfs_extent_pointer_page_valid(
    struct super_block *sb, const u8 block[INFILFS_DISK_BLOCK_SIZE],
    const u8 owner_id[16], u32 expected_level,
    const __le64 **pointers_out, u32 *count_out);
int infilfs_extent_layout_validate(
    struct super_block *sb, const u8 object[INFILFS_DISK_BLOCK_SIZE],
    u32 *page_count_out);
int infilfs_extent_page_block(
    struct super_block *sb, const u8 object[INFILFS_DISK_BLOCK_SIZE],
    u32 page_index, u64 *block_out);
int infilfs_extent_pointer_tree_build(
    struct infilfs_native_pending *pending, const u8 owner_id[16],
    const u64 *extent_pages, u32 page_count,
    u64 *root_out, u32 *levels_out);
int infilfs_extent_layout_defer_free(
    struct infilfs_native_pending *pending,
    const u8 object[INFILFS_DISK_BLOCK_SIZE]);
u32 infilfs_extent_kind(u32 flags);
u64 infilfs_extent_physical_blocks(u32 logical_blocks, u32 flags);
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
int infilfs_index_lookup(
    struct super_block *sb, const u8 object_id[16],
    u64 *object_block_out, u16 *type_out);
bool infilfs_native_checksum_cache_lookup(
    struct super_block *sb, const u8 owner_id[16],
    struct infilfs_native_checksum_cache_entry *out);
void infilfs_native_checksum_cache_store(
    struct super_block *sb, const u8 owner_id[16], const u8 object_id[16],
    u64 object_block, u64 start_logical);
void infilfs_native_checksum_cache_invalidate_sb(struct super_block *sb);
bool infilfs_native_checksum_group_cache_lookup(
    struct super_block *sb, const u8 owner_id[16], u64 start_logical,
    struct infilfs_native_checksum_cache_entry *out);
void infilfs_native_checksum_group_cache_store(
    struct super_block *sb, const u8 owner_id[16], const u8 object_id[16],
    u64 object_block, u64 start_logical);
int infilfs_native_random_id(u8 id[16]);
int infilfs_native_checksum_decode(
    struct super_block *sb, const u8 owner_id[16], const u8 object_id[16],
    u64 object_block, u8 block[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_native_checksum_payload_disk **payload_out);
int infilfs_native_checksum_update_existing_group(
    struct infilfs_native_pending *pending,
    struct infilfs_file_payload_disk *file, const u8 owner_id[16],
    u64 touched_start, u64 touched_count,
    const struct infilfs_data_checksum_disk *digests,
    struct infilfs_native_index_change *changes, u32 change_capacity,
    u32 *change_count, u8 final_tail_id[16], u64 *final_tail_block,
    u64 *final_tail_start);
int infilfs_native_checksum_set_range(
    struct infilfs_native_pending *pending,
    struct infilfs_file_payload_disk *file, const u8 owner_id[16],
    u64 touched_start, u64 touched_count,
    const struct infilfs_data_checksum_disk *digests,
    struct infilfs_native_index_change *changes, u32 change_capacity,
    u32 *change_count, u8 final_tail_id[16], u64 *final_tail_block,
    u64 *final_tail_start);
int infilfs_native_checksum_append_tail(
    struct infilfs_native_pending *pending,
    struct infilfs_file_payload_disk *file, const u8 owner_id[16],
    u64 touched_start, u64 touched_count,
    const struct infilfs_data_checksum_disk *digests,
    struct infilfs_native_index_change *changes, u32 change_capacity,
    u32 *change_count, u8 final_tail_id[16], u64 *final_tail_block,
    u64 *final_tail_start);
int infilfs_native_checksum_append(
    struct infilfs_native_pending *pending,
    struct infilfs_file_payload_disk *file, const u8 owner_id[16],
    u64 touched_start, u64 touched_count,
    const struct infilfs_data_checksum_disk *digests,
    struct infilfs_native_index_change *changes, u32 change_capacity,
    u32 *change_count, u8 final_tail_id[16], u64 *final_tail_block,
    u64 *final_tail_start);
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
ssize_t infilfs_native_writeback_iter(
    struct inode *inode, loff_t *position, struct iov_iter *from,
    size_t requested);
int infilfs_quota_reserve_inode(
    struct inode *inode, u64 bytes, u64 objects,
    struct infilfs_quota_reservation *reservation);
void infilfs_quota_reservation_finish(
    struct infilfs_quota_reservation *reservation,
    u64 actual_bytes, u64 actual_objects);
void infilfs_quota_reservation_abort(
    struct infilfs_quota_reservation *reservation);
extern const struct address_space_operations infilfs_aops;

/* Services shared with the compiled Format 0.18 directory-tree layer. */
extern const u8 infilfs_object_magic[8];
extern const u8 infilfs_directory_page_magic[8];
extern const u8 infilfs_directory_branch_page_magic[8];
void infilfs_rw_sha256_init(struct infilfs_rw_sha256_ctx *ctx);
void infilfs_rw_sha256_update(
    struct infilfs_rw_sha256_ctx *ctx, const u8 *data, size_t length);
void infilfs_rw_sha256_final(struct infilfs_rw_sha256_ctx *ctx, u8 out[32]);
bool infilfs_rw_utf8_valid(const u8 *s, size_t len);
void infilfs_rw_init_page(
    u8 *block, const u8 magic[8], const u8 owner[16], u64 generation);
int infilfs_rw_finalize_page(u8 block[INFILFS_DISK_BLOCK_SIZE]);
int infilfs_rw_finalize_object(u8 block[INFILFS_DISK_BLOCK_SIZE]);
int infilfs_walk_dir_buffer(
    const u8 *buffer, u32 bytes,
    int (*visitor)(const struct infilfs_dirent_disk *, const u8 *, void *),
    void *arg);
int infilfs_native_stage_block(struct super_block *sb, u64 block, const void *data);
int infilfs_native_store_private_or_cow(
    struct infilfs_native_pending *pending, u64 old_block,
    const u8 data[INFILFS_DISK_BLOCK_SIZE], u64 *new_block_out);
void infilfs_native_directory_locator_invalidate(struct infilfs_native_pending *pending);
int infilfs_tree_dir_lookup_name(
    struct inode *dir, const u8 *name, u16 name_len,
    struct infilfs_dir_lookup *search);
int infilfs_tree_dir_for_each(
    struct inode *inode,
    int (*visitor)(const struct infilfs_dirent_disk *, const u8 *, void *),
    void *arg);
/* Pure Linux sidecar metadata codec; no namespace or locking ownership. */
void infilfs_linux_meta_uuid(const u8 id[16], char out[37]);
void infilfs_linux_meta_init(struct infilfs_linux_meta_header *header);
int infilfs_linux_meta_validate_blob(const u8 *blob, size_t size);
int infilfs_linux_meta_find_xattr(
    const u8 *blob, size_t size, const char *name, size_t *offset_out,
    size_t *record_size_out, size_t *value_offset_out,
    size_t *value_length_out);
int infilfs_linux_xattr_name(
    const struct xattr_handler *handler, const char *name, char **full_out);

int infilfs_native_tree_directory_update(
    struct infilfs_native_pending *pending, struct inode *dir,
    const struct qstr *remove_a, const struct qstr *remove_b,
    const struct qstr *add_name, const u8 add_id[16], u16 add_type,
    int link_delta, u64 *new_dir_block_out);

#endif /* INFILTRATORFS_INTERNAL_H */
