// SPDX-License-Identifier: GPL-3.0-or-later
#include <linux/blkdev.h>
#include <linux/bitops.h>
#include <linux/buffer_head.h>
#include <linux/capability.h>
#include <linux/dirent.h>
#include <linux/falloc.h>
#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/cred.h>
#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/seq_file.h>
#include <linux/sort.h>
#include <linux/spinlock.h>
#include <linux/pagevec.h>
#include <linux/pagemap.h>
#include <linux/sched.h>
#include <linux/timekeeping.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>
#include <linux/vmalloc.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uio.h>
#include <linux/version.h>
#include <linux/writeback.h>
#include <linux/xattr.h>

#include "infiltratorfs_format.h"
#include "infiltratorfs_ioctl.h"

#define INFILTRATORFS_NAME "infiltratorfs"
#define INFILTRATORFS_MAGIC 0x494e4653u

static const u8 infilfs_disk_magic[8] = {
    'I', 'N', 'F', 'S', '2', '0', '2', '6'
};
static const u8 infilfs_object_magic[8] = {
    'I', 'N', 'F', 'O', 'B', 'J', '0', '1'
};
static const u8 infilfs_directory_page_magic[8] = {
    'I', 'N', 'F', 'S', 'D', 'P', '0', '1'
};
static const u8 infilfs_directory_branch_page_magic[8] = {
    'I', 'N', 'F', 'S', 'D', 'B', '0', '1'
};
static const u8 infilfs_index_page_magic[8] = {
    'I', 'N', 'F', 'S', 'I', 'P', '0', '1'
};
static const u8 infilfs_index_branch_page_magic[8] = {
    'I', 'N', 'F', 'S', 'I', 'B', '0', '1'
};
static const u8 infilfs_extent_page_magic[8] = {
    'I', 'N', 'F', 'S', 'E', 'P', '0', '1'
};

#define INFILFS_ALLOCATION_RESERVATION_SHARDS 64u

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

struct infilfs_sb_info {
    struct infilfs_superblock_disk disk;
    u64 device_blocks;
    struct mutex write_lock;
    /* Serialize compound Linux sidecar metadata mutations (xattr/special). */
    struct mutex linux_meta_lock;
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
    /*
     * Volatile next-fit cursors.  These are allocation heuristics only and
     * deliberately never enter the on-disk format; a rollback or remount may
     * move them without affecting correctness.
     */
    u64 data_alloc_hint;
    u64 metadata_alloc_hint;
    /*
     * Volatile allocation reservations let independent writers search and
     * claim disjoint data runs before entering the checkpoint publication
     * critical section.  They never enter the on-disk format and are either
     * consumed by the live transaction or released by the calling writer.
     */
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
    /* Volatile per-object locality cursor for sharded data reservations. */
    u64 data_allocation_hint;
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

#define INFILFS_LINUX_META_DIRECTORY ".infilfs-posix-meta"

static int infilfs_linux_meta_get_special(struct super_block *sb,
                                          const u8 object_id[16],
                                          umode_t *mode, dev_t *rdev);
static bool infilfs_linux_meta_directory_is_internal(struct super_block *sb);
static int infilfs_linux_meta_remove_object(struct super_block *sb,
                                            const u8 object_id[16]);

static int infilfs_tree_dir_lookup_name(
    struct inode *dir, const u8 *name, u16 name_len,
    struct infilfs_dir_lookup *search);
static int infilfs_tree_dir_for_each(
    struct inode *inode,
    int (*visitor)(const struct infilfs_dirent_disk *, const u8 *, void *),
    void *arg);

static const struct inode_operations infilfs_dir_inode_operations;
static const struct inode_operations infilfs_symlink_inode_operations;
static const struct inode_operations infilfs_file_inode_operations;
static const struct file_operations infilfs_dir_operations;
static const struct file_operations infilfs_file_operations;
static const struct address_space_operations infilfs_aops;


static u64 infilfs_rw_crc64_zeroed(const u8 *data, size_t length,
                                    size_t zero_offset, size_t zero_length);
static bool infilfs_rw_utf8_valid(const u8 *s, size_t len);

static bool infilfs_crc64_block_valid(const u8 block[INFILFS_DISK_BLOCK_SIZE],
                                      size_t checksum_offset,
                                      size_t checksum_size)
{
    __le64 stored;
    size_t i;
    if (checksum_size < sizeof(stored) || checksum_offset > INFILFS_DISK_BLOCK_SIZE - checksum_size)
        return false;
    memcpy(&stored, block + checksum_offset, sizeof(stored));
    for (i = sizeof(stored); i < checksum_size; ++i)
        if (block[checksum_offset + i] != 0)
            return false;
    return le64_to_cpu(stored) == infilfs_rw_crc64_zeroed(
        block, INFILFS_DISK_BLOCK_SIZE, checksum_offset, checksum_size);
}

static struct infilfs_sb_info *INFILFS_SB(struct super_block *sb)
{
    return sb->s_fs_info;
}

static const char *infilfs_media_profile_name(
    enum infilfs_media_profile profile)
{
    switch (profile) {
    case INFILFS_MEDIA_ROTATIONAL:
        return "rotational";
    case INFILFS_MEDIA_NONROTATIONAL:
        return "nonrotational";
    case INFILFS_MEDIA_BALANCED:
    default:
        return "balanced";
    }
}

static int infilfs_resolve_media_profile(
    struct super_block *sb, const struct infilfs_fs_context *ctx,
    struct infilfs_sb_info *sbi)
{
    struct request_queue *queue;

    if (!sb || !sb->s_bdev || !sbi)
        return -EINVAL;
    queue = bdev_get_queue(sb->s_bdev);
    if (queue && blk_queue_is_zoned(queue)) {
        pr_err("InfiltratorFS: zoned block devices require zone-aware allocation and are not yet supported\n");
        return -EOPNOTSUPP;
    }

    sbi->media_profile_overridden =
        ctx && ctx->media_override != INFILFS_MEDIA_OVERRIDE_AUTO;
    if (ctx) {
        switch (ctx->media_override) {
        case INFILFS_MEDIA_OVERRIDE_BALANCED:
            sbi->media_profile = INFILFS_MEDIA_BALANCED;
            return 0;
        case INFILFS_MEDIA_OVERRIDE_ROTATIONAL:
            sbi->media_profile = INFILFS_MEDIA_ROTATIONAL;
            return 0;
        case INFILFS_MEDIA_OVERRIDE_NONROTATIONAL:
            sbi->media_profile = INFILFS_MEDIA_NONROTATIONAL;
            return 0;
        case INFILFS_MEDIA_OVERRIDE_AUTO:
        default:
            break;
        }
    }

    if (!queue)
        sbi->media_profile = INFILFS_MEDIA_BALANCED;
    else if (blk_queue_rot(queue))
        sbi->media_profile = INFILFS_MEDIA_ROTATIONAL;
    else
        sbi->media_profile = INFILFS_MEDIA_NONROTATIONAL;
    return 0;
}

static bool infilfs_block_allocated(struct super_block *sb, u64 block)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    const u8 *bitmap;
    size_t bytes;
    bool allocated = false;

    if (!sbi || block >= sbi->device_blocks)
        return false;
    read_lock(&sbi->bitmap_lock);
    if (sbi->validation_bitmap) {
        bitmap = sbi->validation_bitmap;
        bytes = sbi->validation_bitmap_bytes;
    } else {
        bitmap = sbi->visible_bitmap;
        bytes = sbi->visible_bitmap_bytes;
    }
    if (bitmap && (block >> 3) < bytes)
        allocated = (READ_ONCE(bitmap[block >> 3]) &
                     (u8)(1u << (block & 7u))) != 0;
    read_unlock(&sbi->bitmap_lock);
    return allocated;
}

static struct infilfs_inode_info *INFILFS_I(struct inode *inode)
{
    return inode->i_private;
}

/*
 * Full metadata-tree walks must never revisit the same physical block.
 * Checksums authenticate contents, but they do not make a cyclic or heavily
 * aliased graph safe to traverse.  Keep a compact per-walk hash set so corrupt
 * media fails with EFSCORRUPTED instead of multiplying recursive work until a
 * kernel thread appears hung.
 */
struct infilfs_visit_set {
    u64 *slots;
    size_t capacity;
    size_t count;
};

static size_t infilfs_visit_hash(u64 key)
{
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return (size_t)key;
}

static int infilfs_visit_grow(struct infilfs_visit_set *set)
{
    u64 *grown;
    size_t next;
    size_t i;

    if (!set)
        return -EINVAL;
    next = set->capacity ? set->capacity * 2u : 64u;
    if (next < set->capacity || next > SIZE_MAX / sizeof(*grown))
        return -EOVERFLOW;
    grown = kvzalloc(next * sizeof(*grown), GFP_NOFS);
    if (!grown)
        return -ENOMEM;

    for (i = 0; i < set->capacity; ++i) {
        u64 key = set->slots[i];
        size_t slot;

        if (!key)
            continue;
        slot = infilfs_visit_hash(key) & (next - 1u);
        while (grown[slot])
            slot = (slot + 1u) & (next - 1u);
        grown[slot] = key;
    }
    kvfree(set->slots);
    set->slots = grown;
    set->capacity = next;
    return 0;
}

static int infilfs_visit_claim(struct infilfs_visit_set *set, u64 block)
{
    u64 key;
    size_t slot;
    int ret;

    if (!set || block == U64_MAX)
        return -EFSCORRUPTED;
    if (!set->capacity || set->count >= set->capacity / 2u) {
        ret = infilfs_visit_grow(set);
        if (ret)
            return ret;
    }

    key = block + 1u; /* zero is the empty-slot sentinel */
    slot = infilfs_visit_hash(key) & (set->capacity - 1u);
    for (;;) {
        if (!set->slots[slot]) {
            set->slots[slot] = key;
            set->count++;
            return 0;
        }
        if (set->slots[slot] == key)
            return -EFSCORRUPTED;
        slot = (slot + 1u) & (set->capacity - 1u);
    }
}

static void infilfs_visit_destroy(struct infilfs_visit_set *set)
{
    if (!set)
        return;
    kvfree(set->slots);
    memset(set, 0, sizeof(*set));
}

static bool infilfs_inode_is_new(struct inode *inode)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
    return (inode_state_read_once(inode) & I_NEW) != 0;
#else
    return (READ_ONCE(inode->i_state) & I_NEW) != 0;
#endif
}

static int infilfs_read_block(struct super_block *sb, u64 block, void *out)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct buffer_head *bh;

    if (!sbi || block >= sbi->device_blocks)
        return -EIO;

    bh = sb_bread(sb, (sector_t)block);
    if (!bh)
        return -EIO;
    memcpy(out, bh->b_data, INFILFS_DISK_BLOCK_SIZE);
    brelse(bh);
    return 0;
}

static int infilfs_read_allocated_block(struct super_block *sb, u64 block,
                                        void *out)
{
    if (!infilfs_block_allocated(sb, block))
        return -EFSCORRUPTED;
    return infilfs_read_block(sb, block, out);
}

#include "infiltratorfs_allocation_map.inc"

static bool infilfs_checkpoint_basic_valid(
    const struct infilfs_superblock_disk *disk, u64 device_blocks)
{
    const u8 *label_end;
    u64 total;
    u64 incompat;
    u64 expected[INFILFS_CHECKPOINT_COUNT];
    unsigned int i;

    if (memcmp(disk->magic, infilfs_disk_magic, sizeof(infilfs_disk_magic)) != 0)
        return false;
    if (le16_to_cpu(disk->format_major) != INFILFS_FORMAT_MAJOR ||
        le16_to_cpu(disk->format_minor) != INFILFS_FORMAT_MINOR ||
        le16_to_cpu(disk->header_size) != sizeof(*disk) ||
        le16_to_cpu(disk->block_shift) != INFILFS_DISK_BLOCK_SHIFT ||
        le32_to_cpu(disk->checksum_type) != INFILFS_CHECKSUM_CRC64_ECMA ||
        le64_to_cpu(disk->generation) == 0)
        return false;

    total = le64_to_cpu(disk->total_blocks);
    if (total != device_blocks || total < 3 ||
        le64_to_cpu(disk->free_blocks) > total)
        return false;

    expected[0] = 0;
    expected[1] = total / 2;
    expected[2] = total - 1;
    for (i = 0; i < INFILFS_CHECKPOINT_COUNT; ++i)
        if (le64_to_cpu(disk->checkpoint_block[i]) != expected[i])
            return false;

    {
        size_t leaves = 0;
        if (infilfs_allocation_counts(total, &leaves, NULL, NULL, NULL) ||
            le64_to_cpu(disk->allocation_leaf_count) != leaves ||
            !le64_to_cpu(disk->allocation_root_block) ||
            le64_to_cpu(disk->allocation_root_block) >= total ||
            le64_to_cpu(disk->object_index_block) >= total ||
            le64_to_cpu(disk->root_object_block) >= total)
            return false;
    }

    incompat = le64_to_cpu(disk->incompat_flags);
    if (incompat & ~INFILFS_KNOWN_INCOMPAT_FLAGS)
        return false;
    if ((incompat & (INFILFS_INCOMPAT_UTF8_NAMES |
                     INFILFS_INCOMPAT_SPARSE_EXTENTS |
                     INFILFS_INCOMPAT_ALLOCATION_TREE)) !=
        (INFILFS_INCOMPAT_UTF8_NAMES | INFILFS_INCOMPAT_SPARSE_EXTENTS |
         INFILFS_INCOMPAT_ALLOCATION_TREE))
        return false;
    if (le64_to_cpu(disk->compat_flags) != 0 ||
        le64_to_cpu(disk->ro_compat_flags) != 0)
        return false;
    if (!memchr_inv(disk->filesystem_uuid, 0, sizeof(disk->filesystem_uuid)) ||
        !memchr_inv(disk->root_object_id, 0, sizeof(disk->root_object_id)))
        return false;
    label_end = memchr(disk->label, 0, sizeof(disk->label));
    if (!label_end ||
        memchr_inv(label_end, 0,
                   sizeof(disk->label) - (size_t)(label_end - disk->label)) ||
        !infilfs_rw_utf8_valid(disk->label,
                               (size_t)(label_end - disk->label)))
        return false;

    return true;
}

struct infilfs_checkpoint_candidates {
    struct infilfs_superblock_disk disks[INFILFS_CHECKPOINT_COUNT];
    bool valid[INFILFS_CHECKPOINT_COUNT];
    int first_read_error;
};

static int infilfs_read_checkpoint_candidates(
    struct super_block *sb, struct infilfs_checkpoint_candidates *set)
{
    u64 blocks = INFILFS_SB(sb)->device_blocks;
    u64 candidates[INFILFS_CHECKPOINT_COUNT];
    struct infilfs_superblock_disk current;
    unsigned int found = 0;
    unsigned int i;
    u8 *raw;

    memset(set, 0, sizeof(*set));
    candidates[0] = 0;
    candidates[1] = blocks / 2;
    candidates[2] = blocks - 1;

    raw = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!raw)
        return -ENOMEM;

    for (i = 0; i < INFILFS_CHECKPOINT_COUNT; ++i) {
        int ret = infilfs_read_block(sb, candidates[i], raw);

        if (ret) {
            if (!set->first_read_error)
                set->first_read_error = ret;
            continue;
        }
        if (!infilfs_crc64_block_valid(raw,
                offsetof(struct infilfs_superblock_disk, checksum),
                sizeof(((struct infilfs_superblock_disk *)raw)->checksum)))
            continue;
        if (memchr_inv(raw + sizeof(current), 0,
                       INFILFS_DISK_BLOCK_SIZE - sizeof(current)))
            continue;
        memcpy(&current, raw, sizeof(current));
        if (!infilfs_checkpoint_basic_valid(&current, blocks))
            continue;
        set->disks[i] = current;
        set->valid[i] = true;
        found++;
    }

    kfree(raw);
    /* A writable mount must never heal over an unreadable checkpoint: that
     * location may contain the only durable newer generation after a crash
     * between primary publication and replica refresh. */
    if (set->first_read_error && !sb_rdonly(sb))
        return set->first_read_error;
    if (found)
        return 0;
    return set->first_read_error ? set->first_read_error : -EFSCORRUPTED;
}

static bool infilfs_object_basic_valid(struct super_block *sb,
                                       const u8 *block,
                                       u16 expected_type,
                                       const u8 *expected_id)
{
    const struct infilfs_object_header_disk *header =
        (const struct infilfs_object_header_disk *)block;
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    u16 version;
    u32 payload;

    if (memcmp(header->magic, infilfs_object_magic,
               sizeof(infilfs_object_magic)) != 0)
        return false;
    if (!infilfs_crc64_block_valid(block,
            offsetof(struct infilfs_object_header_disk, checksum),
            sizeof(header->checksum)))
        return false;
    if (le32_to_cpu(header->header_size) != sizeof(*header) ||
        le32_to_cpu(header->checksum_type) != INFILFS_CHECKSUM_CRC64_ECMA)
        return false;
    if (le64_to_cpu(header->generation) == 0 ||
        le64_to_cpu(header->generation) > le64_to_cpu(sbi->disk.generation))
        return false;

    if (expected_type && le16_to_cpu(header->object_type) != expected_type)
        return false;
    if (expected_id && memcmp(header->object_id, expected_id, 16) != 0)
        return false;

    version = le16_to_cpu(header->object_version);
    if (version != INFILFS_OBJECT_VERSION_CLASSIC &&
        version != INFILFS_OBJECT_VERSION_PAGED &&
        !(version == INFILFS_OBJECT_VERSION_TREE &&
          (le16_to_cpu(header->object_type) == INFILFS_OBJECT_INDEX ||
           le16_to_cpu(header->object_type) == INFILFS_OBJECT_DIRECTORY)))
        return false;

    payload = le32_to_cpu(header->payload_size);
    if (payload > INFILFS_DISK_BLOCK_SIZE - sizeof(*header))
        return false;
    return true;
}

static int infilfs_read_object(struct super_block *sb, u64 object_block,
                               u16 expected_type, const u8 *expected_id,
                               u8 *out)
{
    int ret;

    ret = infilfs_read_allocated_block(sb, object_block, out);

    if (ret)
        return ret;
    if (!infilfs_object_basic_valid(sb, out, expected_type, expected_id))
        return -EFSCORRUPTED;
    return 0;
}

static bool infilfs_metadata_page_valid(struct super_block *sb,
                                        const u8 *block,
                                        const u8 magic[8],
                                        const u8 owner_id[16])
{
    const struct infilfs_metadata_page_disk *page =
        (const struct infilfs_metadata_page_disk *)block;
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    u32 bytes;

    if (memcmp(page->magic, magic, 8) != 0 ||
        memcmp(page->owner_object_id, owner_id, 16) != 0)
        return false;
    if (!infilfs_crc64_block_valid(block,
            offsetof(struct infilfs_metadata_page_disk, checksum),
            sizeof(page->checksum)))
        return false;
    if (le64_to_cpu(page->generation) == 0 ||
        le64_to_cpu(page->generation) > le64_to_cpu(sbi->disk.generation))
        return false;
    if (le32_to_cpu(page->checksum_type) != INFILFS_CHECKSUM_CRC64_ECMA ||
        le32_to_cpu(page->reserved) != 0)
        return false;

    bytes = le32_to_cpu(page->bytes_used);
    if (bytes > INFILFS_DISK_BLOCK_SIZE - sizeof(*page))
        return false;
    return true;
}

#include "infiltratorfs_index_tree.inc"

static u64 infilfs_object_ino(const u8 id[16])
{
    u64 hash = 1469598103934665603ULL;
    unsigned int i;

    for (i = 0; i < 16; ++i) {
        hash ^= id[i];
        hash *= 1099511628211ULL;
    }
    hash &= 0x7fffffffffffffffULL;
    if (hash < 3)
        hash += 3;
    return hash;
}

static unsigned int infilfs_dtype(u16 type)
{
    switch (type) {
    case INFILFS_OBJECT_DIRECTORY:
        return DT_DIR;
    case INFILFS_OBJECT_FILE:
        return DT_REG;
    case INFILFS_OBJECT_SYMLINK:
        return DT_LNK;
    default:
        return DT_UNKNOWN;
    }
}

static int infilfs_index_lookup_indexed(struct super_block *sb,
                                        const u8 object_id[16],
                                        u64 *object_block_out,
                                        u16 *type_out)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    const struct infilfs_object_header_disk *header;
    const struct infilfs_index_payload_disk *payload;
    const struct infilfs_index_entry_disk *entries;
    u32 total_entries;
    u16 version;
    u8 *head;
    u8 *page_block;
    int ret = -ENOENT;
    u32 i;

    head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!head || !page_block) {
        ret = -ENOMEM;
        goto out;
    }

    ret = infilfs_read_object(sb, le64_to_cpu(sbi->disk.object_index_block),
                              INFILFS_OBJECT_INDEX, NULL, head);
    if (ret)
        goto out;

    header = (const struct infilfs_object_header_disk *)head;
    payload = (const struct infilfs_index_payload_disk *)(header + 1);
    if (le32_to_cpu(header->payload_size) < sizeof(*payload)) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    total_entries = le32_to_cpu(payload->entry_count);
    version = le16_to_cpu(header->object_version);

    if (version == INFILFS_OBJECT_VERSION_CLASSIC) {
        size_t needed = sizeof(*payload) +
            (size_t)total_entries * sizeof(struct infilfs_index_entry_disk);

        if (le32_to_cpu(payload->reserved) != 0 ||
            needed > le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        entries = (const struct infilfs_index_entry_disk *)(payload + 1);
        for (i = 0; i < total_entries; ++i) {
            if (memcmp(entries[i].object_id, object_id, 16) != 0)
                continue;
            *object_block_out = le64_to_cpu(entries[i].object_block);
            *type_out = le16_to_cpu(entries[i].object_type);
            ret = 0;
            goto out;
        }
        ret = -ENOENT;
        goto out;
    }

    if (version == INFILFS_OBJECT_VERSION_TREE) {
        ret = infilfs_index_tree_lookup_head(
            sb, head, object_id, object_block_out, type_out);
        goto out;
    }

    if (version == INFILFS_OBJECT_VERSION_PAGED) {
        const __le64 *pages = (const __le64 *)(payload + 1);
        u32 page_count = le32_to_cpu(payload->reserved);
        u32 seen = 0;
        u32 p;

        if ((total_entries && page_count == 0) ||
            page_count > INFILFS_INDEX_PAGE_POINTERS ||
            sizeof(*payload) + (size_t)page_count * sizeof(*pages) >
                le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        for (p = 0; p < page_count; ++p) {
            u64 block_no = le64_to_cpu(pages[p]);
            const struct infilfs_metadata_page_disk *page;
            u32 count;

            if (!infilfs_block_allocated(sb, block_no)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            ret = infilfs_read_block(sb, block_no, page_block);
            if (ret)
                goto out;
            if (!infilfs_metadata_page_valid(sb, page_block,
                                             infilfs_index_page_magic,
                                             header->object_id)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            page = (const struct infilfs_metadata_page_disk *)page_block;
            count = le32_to_cpu(page->entry_count);
            if (count > INFILFS_INDEX_ENTRIES_PER_PAGE ||
                le32_to_cpu(page->bytes_used) !=
                    count * sizeof(struct infilfs_index_entry_disk)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            entries = (const struct infilfs_index_entry_disk *)(page + 1);
            for (i = 0; i < count; ++i) {
                if (memcmp(entries[i].object_id, object_id, 16) != 0)
                    continue;
                *object_block_out = le64_to_cpu(entries[i].object_block);
                *type_out = le16_to_cpu(entries[i].object_type);
                ret = 0;
                goto out;
            }
            seen += count;
        }
        ret = seen == total_entries ? -ENOENT : -EFSCORRUPTED;
        goto out;
    }

    ret = -EFSCORRUPTED;
out:
    kfree(page_block);
    kfree(head);
    return ret;
}

static int infilfs_index_lookup(struct super_block *sb, const u8 object_id[16],
                                u64 *object_block_out, u16 *type_out)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);

    if (memcmp(object_id, sbi->disk.root_object_id, 16) == 0) {
        *object_block_out = le64_to_cpu(sbi->disk.root_object_block);
        *type_out = INFILFS_OBJECT_DIRECTORY;
        return 0;
    }
    return infilfs_index_lookup_indexed(sb, object_id, object_block_out,
                                        type_out);
}

static bool infilfs_checkpoint_bitmap_get(const u8 *bitmap, u64 block)
{
    return (bitmap[block >> 3] & (u8)(1u << (block & 7u))) != 0;
}

struct infilfs_checkpoint_tree_node {
    u64 block;
    u32 depth;
};

static int infilfs_validate_checkpoint_directory_tree(
    struct super_block *sb,
    const u8 object[INFILFS_DISK_BLOCK_SIZE])
{
    const struct infilfs_object_header_disk *header =
        (const struct infilfs_object_header_disk *)object;
    const struct infilfs_directory_payload_disk *payload =
        (const struct infilfs_directory_payload_disk *)(header + 1);
    struct infilfs_checkpoint_tree_node *nodes = NULL;
    struct infilfs_visit_set visited = {0};
    u32 expected_entries = le32_to_cpu(payload->entry_count);
    u64 seen_entries = 0;
    size_t capacity = 0;
    size_t node_count = 0;
    size_t cursor = 0;
    u64 root;
    u8 *block;
    int ret = 0;

    if (le16_to_cpu(header->object_version) != INFILFS_OBJECT_VERSION_TREE ||
        le32_to_cpu(payload->bytes_used) != 0 ||
        le32_to_cpu(header->payload_size) !=
            sizeof(*payload) + sizeof(__le64))
        return -EFSCORRUPTED;
    {
        __le64 root_le;

        memcpy(&root_le, payload + 1, sizeof(root_le));
        root = le64_to_cpu(root_le);
    }
    if (!expected_entries)
        return root ? -EFSCORRUPTED : 0;
    if (!root || !infilfs_block_allocated(sb, root))
        return -EFSCORRUPTED;

    block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    nodes = kvmalloc_array(64u, sizeof(*nodes), GFP_NOFS);
    if (!block || !nodes) {
        ret = -ENOMEM;
        goto out;
    }
    ret = infilfs_visit_claim(&visited, root);
    if (ret)
        goto out;
    capacity = 64u;
    nodes[node_count++] = (struct infilfs_checkpoint_tree_node) {
        .block = root,
        .depth = 0,
    };

    while (cursor < node_count) {
        const struct infilfs_checkpoint_tree_node current = nodes[cursor++];
        const struct infilfs_metadata_page_disk *page;

        ret = infilfs_read_allocated_block(sb, current.block, block);
        if (ret)
            goto out;
        page = (const struct infilfs_metadata_page_disk *)block;

        if (!memcmp(block, infilfs_directory_page_magic, 8)) {
            const u8 *entries;
            u32 bytes;
            u32 offset = 0;
            u32 count = 0;

            if (!infilfs_metadata_page_valid(
                    sb, block, infilfs_directory_page_magic,
                    header->object_id)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            bytes = le32_to_cpu(page->bytes_used);
            entries = (const u8 *)(page + 1);
            while (offset < bytes) {
                const struct infilfs_dirent_disk *entry;
                const u8 *name;
                u16 record_size;
                u16 name_length;
                u16 type;

                if (bytes - offset < sizeof(*entry)) {
                    ret = -EFSCORRUPTED;
                    goto out;
                }
                entry = (const struct infilfs_dirent_disk *)(entries + offset);
                record_size = le16_to_cpu(entry->record_size);
                name_length = le16_to_cpu(entry->name_length);
                type = le16_to_cpu(entry->object_type);
                if (record_size < sizeof(*entry) || (record_size & 7u) ||
                    record_size > bytes - offset || !name_length ||
                    name_length > INFILFS_NAME_MAX ||
                    sizeof(*entry) + name_length > record_size ||
                    le16_to_cpu(entry->flags) != 0 ||
                    (type != INFILFS_OBJECT_DIRECTORY &&
                     type != INFILFS_OBJECT_FILE &&
                     type != INFILFS_OBJECT_SYMLINK) ||
                    !memchr_inv(entry->object_id, 0, 16)) {
                    ret = -EFSCORRUPTED;
                    goto out;
                }
                name = entries + offset + sizeof(*entry);
                if (!infilfs_rw_utf8_valid(name, name_length) ||
                    memchr(name, '\0', name_length) ||
                    memchr(name, '/', name_length)) {
                    ret = -EFSCORRUPTED;
                    goto out;
                }
                offset += record_size;
                count++;
            }
            if (offset != bytes || count != le32_to_cpu(page->entry_count) ||
                seen_entries > U64_MAX - count) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            seen_entries += count;
            continue;
        }

        if (current.depth >= INFILFS_DIRECTORY_TREE_DEPTH ||
            !infilfs_metadata_page_valid(
                sb, block, infilfs_directory_branch_page_magic,
                header->object_id) ||
            le32_to_cpu(page->bytes_used) !=
                INFILFS_DIRECTORY_TREE_BRANCH_BYTES ||
            !le32_to_cpu(page->entry_count) ||
            le32_to_cpu(page->entry_count) >
                INFILFS_DIRECTORY_TREE_FANOUT) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        {
            const __le64 *children = (const __le64 *)(page + 1);
            u32 child_count = 0;
            u32 slot;

            for (slot = 0; slot < INFILFS_DIRECTORY_TREE_FANOUT; ++slot) {
                struct infilfs_checkpoint_tree_node *grown;
                u64 child = le64_to_cpu(children[slot]);

                if (!child)
                    continue;
                if (!infilfs_block_allocated(sb, child)) {
                    ret = -EFSCORRUPTED;
                    goto out;
                }
                ret = infilfs_visit_claim(&visited, child);
                if (ret)
                    goto out;
                if (node_count == capacity) {
                    size_t next = capacity * 2u;

                    if (next < capacity ||
                        next > INFILFS_SB(sb)->device_blocks ||
                        next > SIZE_MAX / sizeof(*nodes)) {
                        ret = -EFSCORRUPTED;
                        goto out;
                    }
                    grown = kvmalloc_array(next, sizeof(*nodes), GFP_NOFS);
                    if (!grown) {
                        ret = -ENOMEM;
                        goto out;
                    }
                    memcpy(grown, nodes, node_count * sizeof(*nodes));
                    kvfree(nodes);
                    nodes = grown;
                    capacity = next;
                }
                nodes[node_count++] = (struct infilfs_checkpoint_tree_node) {
                    .block = child,
                    .depth = current.depth + 1u,
                };
                child_count++;
            }
            if (child_count != le32_to_cpu(page->entry_count)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
        }
    }
    if (seen_entries != expected_entries)
        ret = -EFSCORRUPTED;
out:
    infilfs_visit_destroy(&visited);
    kvfree(nodes);
    kfree(block);
    return ret;
}

static int infilfs_validate_checkpoint_extents(
    struct super_block *sb, const struct infilfs_extent_disk *extents,
    u32 count, u64 *last_logical, bool *has_normal)
{
    u32 i;

    for (i = 0; i < count; ++i) {
        u64 logical = le64_to_cpu(extents[i].logical_block);
        u64 physical = le64_to_cpu(extents[i].physical_block);
        u32 blocks = le32_to_cpu(extents[i].block_count);
        u32 flags = le32_to_cpu(extents[i].flags);
        u64 b;

        if (!blocks || logical != *last_logical ||
            logical > U64_MAX - blocks ||
            (flags != INFILFS_EXTENT_NORMAL &&
             flags != INFILFS_EXTENT_HOLE))
            return -EFSCORRUPTED;
        *last_logical = logical + blocks;
        if (flags == INFILFS_EXTENT_HOLE) {
            if (physical)
                return -EFSCORRUPTED;
            continue;
        }
        if (!physical || physical >= INFILFS_SB(sb)->device_blocks ||
            blocks > INFILFS_SB(sb)->device_blocks - physical)
            return -EFSCORRUPTED;
        *has_normal = true;
        for (b = 0; b < blocks; ++b)
            if (!infilfs_block_allocated(sb, physical + b))
                return -EFSCORRUPTED;
    }
    return 0;
}

static int infilfs_validate_checkpoint_file(
    struct super_block *sb,
    const u8 object[INFILFS_DISK_BLOCK_SIZE])
{
    const struct infilfs_object_header_disk *header =
        (const struct infilfs_object_header_disk *)object;
    const struct infilfs_file_payload_disk *file =
        (const struct infilfs_file_payload_disk *)(header + 1);
    u32 extent_count = le32_to_cpu(file->extent_count);
    u16 version = le16_to_cpu(header->object_version);
    u64 last_logical = 0;
    bool has_normal = false;

    if (le32_to_cpu(header->payload_size) < sizeof(*file))
        return -EFSCORRUPTED;
    if (!extent_count) {
        u64 size = le64_to_cpu(file->attributes.logical_size);
        size_t expected = sizeof(*file);

        if (version != INFILFS_OBJECT_VERSION_CLASSIC ||
            size > INFILFS_INLINE_DATA_MAX ||
            le32_to_cpu(file->data_checksum_type) !=
                INFILFS_CHECKSUM_SHA256 ||
            memchr_inv(file->checksum_head_id, 0,
                       sizeof(file->checksum_head_id)))
            return -EFSCORRUPTED;
        if (size)
            expected += sizeof(struct infilfs_data_checksum_disk) +
                (size_t)size;
        return expected == le32_to_cpu(header->payload_size) ?
            0 : -EFSCORRUPTED;
    }
    if (le32_to_cpu(file->data_checksum_type) != INFILFS_CHECKSUM_SHA256)
        return -EFSCORRUPTED;
    if (version == INFILFS_OBJECT_VERSION_CLASSIC) {
        const struct infilfs_extent_disk *extents =
            (const struct infilfs_extent_disk *)(file + 1);

        if (extent_count > (INFILFS_DISK_BLOCK_SIZE - sizeof(*header) -
                            sizeof(*file)) / sizeof(*extents) ||
            sizeof(*file) + (size_t)extent_count * sizeof(*extents) !=
            le32_to_cpu(header->payload_size))
            return -EFSCORRUPTED;
        {
            int ret = infilfs_validate_checkpoint_extents(
                sb, extents, extent_count, &last_logical, &has_normal);
            bool has_checksum_head = memchr_inv(
                file->checksum_head_id, 0,
                sizeof(file->checksum_head_id)) != NULL;

            return ret ? ret :
                (has_normal == has_checksum_head ? 0 : -EFSCORRUPTED);
        }
    }
    if (version == INFILFS_OBJECT_VERSION_PAGED) {
        const struct infilfs_extent_head_disk *head =
            (const struct infilfs_extent_head_disk *)(file + 1);
        const __le64 *pages = (const __le64 *)(head + 1);
        u32 page_count = le32_to_cpu(head->page_count);
        u32 copied = 0;
        u8 *page_block;
        u32 p;
        int ret = 0;

        if (!page_count || page_count > INFILFS_EXTENT_PAGE_POINTERS ||
            le32_to_cpu(head->reserved) != 0 ||
            sizeof(*file) + sizeof(*head) +
                (size_t)page_count * sizeof(*pages) !=
                    le32_to_cpu(header->payload_size))
            return -EFSCORRUPTED;
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!page_block)
            return -ENOMEM;
        for (p = 0; p < page_count; ++p) {
            const struct infilfs_metadata_page_disk *page;
            const struct infilfs_extent_disk *extents;
            u32 count;

            ret = infilfs_read_allocated_block(
                sb, le64_to_cpu(pages[p]), page_block);
            if (ret)
                break;
            if (!infilfs_metadata_page_valid(
                    sb, page_block, infilfs_extent_page_magic,
                    header->object_id)) {
                ret = -EFSCORRUPTED;
                break;
            }
            page = (const struct infilfs_metadata_page_disk *)page_block;
            count = le32_to_cpu(page->entry_count);
            if (!count || count > INFILFS_EXTENTS_PER_PAGE ||
                copied > extent_count || count > extent_count - copied ||
                le32_to_cpu(page->bytes_used) !=
                    count * sizeof(*extents)) {
                ret = -EFSCORRUPTED;
                break;
            }
            extents = (const struct infilfs_extent_disk *)(page + 1);
            ret = infilfs_validate_checkpoint_extents(
                sb, extents, count, &last_logical, &has_normal);
            if (ret)
                break;
            copied += count;
        }
        kfree(page_block);
        if (!ret && copied != extent_count)
            ret = -EFSCORRUPTED;
        if (!ret && has_normal !=
            (memchr_inv(file->checksum_head_id, 0,
                        sizeof(file->checksum_head_id)) != NULL))
            ret = -EFSCORRUPTED;
        return ret;
    }
    return -EFSCORRUPTED;
}

static int infilfs_validate_checkpoint_index_tree(
    struct super_block *sb,
    const u8 index[INFILFS_DISK_BLOCK_SIZE], bool directory_trees)
{
    struct infilfs_index_entry_disk *entries = NULL;
    u8 *object = NULL;
    u32 count = 0;
    u32 i;
    int ret;

    ret = infilfs_index_tree_snapshot(sb, index, &entries, &count);
    if (ret)
        return ret;
    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object) {
        ret = -ENOMEM;
        goto out;
    }
    for (i = 0; i < count; ++i) {
        u16 type = le16_to_cpu(entries[i].object_type);

        ret = infilfs_read_object(
            sb, le64_to_cpu(entries[i].object_block), type,
            entries[i].object_id, object);
        if (ret)
            goto out;
        if (type == INFILFS_OBJECT_DIRECTORY && directory_trees) {
            ret = infilfs_validate_checkpoint_directory_tree(sb, object);
            if (ret)
                goto out;
        } else if (type == INFILFS_OBJECT_FILE) {
            ret = infilfs_validate_checkpoint_file(sb, object);
            if (ret)
                goto out;
        }
    }
out:
    kfree(object);
    kvfree(entries);
    return ret;
}

static int infilfs_validate_checkpoint_graph(
    struct super_block *sb, const struct infilfs_superblock_disk *candidate,
    bool deep_validation)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    const struct infilfs_object_header_disk *root_header;
    const struct infilfs_object_header_disk *index_header;
    const struct infilfs_directory_payload_disk *root_payload;
    u64 indexed_root;
    u16 indexed_type;
    size_t bitmap_bytes = 0;
    u8 *bitmap = NULL;
    u8 *root = NULL;
    u8 *index = NULL;
    bool paged_feature;
    bool index_tree_feature;
    bool directory_tree_feature;
    bool root_paged;
    bool root_tree;
    bool index_paged;
    bool index_tree;
    int ret = 0;

    /* Object/page validators use sbi->disk as their generation and feature
     * context. Install each candidate before walking its graph exactly as the
     * pre-Format-0.17 validator did; a later candidate overwrites this context
     * if the current one is rejected. */
    sbi->disk = *candidate;

    ret = infilfs_allocation_map_load(
        sb, candidate, &bitmap, &bitmap_bytes, NULL);
    if (ret)
        goto out;

    root = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    index = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!root || !index) {
        ret = -ENOMEM;
        goto out;
    }

    write_lock(&sbi->bitmap_lock);
    sbi->validation_bitmap = bitmap;
    sbi->validation_bitmap_bytes = bitmap_bytes;
    write_unlock(&sbi->bitmap_lock);

    ret = infilfs_read_object(sb, le64_to_cpu(candidate->root_object_block),
                              INFILFS_OBJECT_DIRECTORY,
                              candidate->root_object_id, root);
    if (ret)
        goto out;
    root_header = (const struct infilfs_object_header_disk *)root;
    root_payload = (const struct infilfs_directory_payload_disk *)(root_header + 1);
    if (memchr_inv(root_header->parent_id, 0, sizeof(root_header->parent_id)) ||
        le32_to_cpu(root_header->payload_size) < sizeof(*root_payload)) {
        ret = -EFSCORRUPTED;
        goto out;
    }

    ret = infilfs_read_object(sb, le64_to_cpu(candidate->object_index_block),
                              INFILFS_OBJECT_INDEX, NULL, index);
    if (ret)
        goto out;
    index_header = (const struct infilfs_object_header_disk *)index;
    paged_feature = (le64_to_cpu(candidate->incompat_flags) &
                     INFILFS_INCOMPAT_PAGED_METADATA) != 0;
    index_tree_feature = (le64_to_cpu(candidate->incompat_flags) &
                          INFILFS_INCOMPAT_INDEX_TREE) != 0;
    directory_tree_feature = (le64_to_cpu(candidate->incompat_flags) &
                              INFILFS_INCOMPAT_DIRECTORY_TREE) != 0;
    root_paged = le16_to_cpu(root_header->object_version) ==
        INFILFS_OBJECT_VERSION_PAGED;
    root_tree = le16_to_cpu(root_header->object_version) ==
        INFILFS_OBJECT_VERSION_TREE;
    index_paged = le16_to_cpu(index_header->object_version) ==
        INFILFS_OBJECT_VERSION_PAGED;
    index_tree = le16_to_cpu(index_header->object_version) ==
        INFILFS_OBJECT_VERSION_TREE;
    if (directory_tree_feature != root_tree ||
        (!directory_tree_feature && paged_feature != root_paged) ||
        index_tree_feature != index_tree ||
        (!index_tree_feature && paged_feature != index_paged)) {
        ret = -EFSCORRUPTED;
        goto out;
    }

    ret = infilfs_index_lookup_indexed(sb, candidate->root_object_id,
                                       &indexed_root, &indexed_type);
    if (ret == -ENOENT)
        ret = -EFSCORRUPTED;
    if (!ret && (indexed_root != le64_to_cpu(candidate->root_object_block) ||
                 indexed_type != INFILFS_OBJECT_DIRECTORY))
        ret = -EFSCORRUPTED;
    if (!ret && deep_validation && index_tree)
        ret = infilfs_validate_checkpoint_index_tree(
            sb, index, directory_tree_feature);
out:
    write_lock(&sbi->bitmap_lock);
    sbi->validation_bitmap = NULL;
    sbi->validation_bitmap_bytes = 0;
    write_unlock(&sbi->bitmap_lock);
    kfree(index);
    kfree(root);
    kvfree(bitmap);
    return ret;
}

static int infilfs_select_checkpoint(struct super_block *sb,
                                     struct infilfs_superblock_disk *selected)
{
    struct infilfs_checkpoint_candidates *set;
    bool tried[INFILFS_CHECKPOINT_COUNT] = { false };
    bool deep_validation = false;
    int ret;
    unsigned int attempt;

    set = kzalloc(sizeof(*set), GFP_KERNEL);
    if (!set)
        return -ENOMEM;
    ret = infilfs_read_checkpoint_candidates(sb, set);
    if (ret)
        goto out;
    for (attempt = 0; attempt < INFILFS_CHECKPOINT_COUNT; ++attempt) {
        if (!set->valid[attempt] || !set->valid[0] ||
            memcmp(&set->disks[attempt], &set->disks[0],
                   sizeof(set->disks[0])) != 0) {
            deep_validation = true;
            break;
        }
    }
    for (attempt = 0; attempt < INFILFS_CHECKPOINT_COUNT; ++attempt) {
        int best = -1;
        u64 best_generation = 0;
        unsigned int i;

        for (i = 0; i < INFILFS_CHECKPOINT_COUNT; ++i) {
            u64 generation;

            if (!set->valid[i] || tried[i])
                continue;
            generation = le64_to_cpu(set->disks[i].generation);
            if (best < 0 || generation > best_generation) {
                best = (int)i;
                best_generation = generation;
            }
        }
        if (best < 0)
            break;
        tried[best] = true;
        ret = infilfs_validate_checkpoint_graph(
            sb, &set->disks[best], deep_validation);
        if (!ret) {
            unsigned int i;

            *selected = set->disks[best];
            INFILFS_SB(sb)->disk = *selected;
            INFILFS_SB(sb)->checkpoint_repair_needed = false;
            for (i = 0; i < INFILFS_CHECKPOINT_COUNT; ++i) {
                if (!set->valid[i] ||
                    memcmp(&set->disks[i], selected, sizeof(*selected)) != 0) {
                    INFILFS_SB(sb)->checkpoint_repair_needed = true;
                    break;
                }
            }
            goto out;
        }
        if (ret != -EFSCORRUPTED)
            goto out;
    }
    ret = -EFSCORRUPTED;
out:
    kfree(set);
    return ret;
}

static int infilfs_walk_dir_buffer(const u8 *buffer, u32 bytes,
                                   int (*visitor)(const struct infilfs_dirent_disk *,
                                                  const u8 *, void *),
                                   void *arg)
{
    u32 offset = 0;

    while (offset < bytes) {
        const struct infilfs_dirent_disk *entry;
        u16 record_size;
        u16 name_len;
        const u8 *name;
        int ret;

        if (bytes - offset < sizeof(*entry))
            return -EFSCORRUPTED;
        entry = (const struct infilfs_dirent_disk *)(buffer + offset);
        record_size = le16_to_cpu(entry->record_size);
        name_len = le16_to_cpu(entry->name_length);
        if (record_size < sizeof(*entry) || (record_size & 7u) != 0 ||
            record_size > bytes - offset || name_len == 0 ||
            name_len > INFILFS_NAME_MAX || sizeof(*entry) + name_len > record_size)
            return -EFSCORRUPTED;

        name = buffer + offset + sizeof(*entry);
        if (memchr(name, '\0', name_len) || memchr(name, '/', name_len))
            return -EFSCORRUPTED;

        ret = visitor(entry, name, arg);
        if (ret)
            return ret;
        offset += record_size;
    }
    return offset == bytes ? 0 : -EFSCORRUPTED;
}

static int infilfs_for_each_dirent(struct inode *inode,
                                   int (*visitor)(const struct infilfs_dirent_disk *,
                                                  const u8 *, void *),
                                   void *arg)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    const struct infilfs_object_header_disk *header;
    const struct infilfs_directory_payload_disk *payload;
    u8 *object;
    u8 *page_block;
    u16 version;
    int ret;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!object || !page_block) {
        ret = -ENOMEM;
        goto out;
    }

    ret = infilfs_read_object(inode->i_sb, ii->object_block,
                              INFILFS_OBJECT_DIRECTORY, ii->object_id, object);
    if (ret)
        goto out;
    header = (const struct infilfs_object_header_disk *)object;
    payload = (const struct infilfs_directory_payload_disk *)(header + 1);
    version = le16_to_cpu(header->object_version);

    if (version == INFILFS_OBJECT_VERSION_CLASSIC) {
        u32 bytes = le32_to_cpu(payload->bytes_used);
        size_t needed = sizeof(*payload) + bytes;

        if (needed > le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        ret = infilfs_walk_dir_buffer((const u8 *)(payload + 1), bytes,
                                      visitor, arg);
        goto out;
    }

    if (version == INFILFS_OBJECT_VERSION_TREE) {
        ret = infilfs_tree_dir_for_each(inode, visitor, arg);
        goto out;
    }

    if (version == INFILFS_OBJECT_VERSION_PAGED) {
        const __le64 *pages = (const __le64 *)(payload + 1);
        u32 page_count = le32_to_cpu(payload->bytes_used);
        u32 p;

        if (page_count == 0) {
            ret = le32_to_cpu(payload->entry_count) == 0 ? 0 : -EFSCORRUPTED;
            goto out;
        }
        if (page_count > INFILFS_DIRECTORY_PAGE_POINTERS) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        for (p = 0; p < page_count; ++p) {
            const struct infilfs_metadata_page_disk *page;
            u32 bytes;

            ret = infilfs_read_allocated_block(
                inode->i_sb, le64_to_cpu(pages[p]), page_block);
            if (ret)
                goto out;
            if (!infilfs_metadata_page_valid(inode->i_sb, page_block,
                                             infilfs_directory_page_magic,
                                             header->object_id)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            page = (const struct infilfs_metadata_page_disk *)page_block;
            bytes = le32_to_cpu(page->bytes_used);
            ret = infilfs_walk_dir_buffer((const u8 *)(page + 1), bytes,
                                          visitor, arg);
            if (ret)
                goto out;
        }
        ret = 0;
        goto out;
    }

    ret = -EFSCORRUPTED;
out:
    kfree(page_block);
    kfree(object);
    return ret;
}

static int infilfs_lookup_visitor(const struct infilfs_dirent_disk *entry,
                                  const u8 *name, void *arg)
{
    struct infilfs_dir_lookup *search = arg;
    u16 name_len = le16_to_cpu(entry->name_length);

    if (name_len != search->name_len || memcmp(name, search->name, name_len) != 0)
        return 0;
    memcpy(search->object_id, entry->object_id, 16);
    search->object_type = le16_to_cpu(entry->object_type);
    search->found = true;
    return 1;
}

static int infilfs_snapshot_visitor(const struct infilfs_dirent_disk *entry,
                                    const u8 *name, void *arg)
{
    struct infilfs_dir_snapshot *snapshot = arg;
    struct infilfs_dir_snapshot_entry *copy;
    u16 name_len = le16_to_cpu(entry->name_length);

    if (!name_len || name_len > INFILFS_NAME_MAX)
        return -EFSCORRUPTED;
    copy = kmalloc(sizeof(*copy) + name_len, GFP_NOFS);
    if (!copy)
        return -ENOMEM;
    INIT_LIST_HEAD(&copy->link);
    memcpy(copy->object_id, entry->object_id, sizeof(copy->object_id));
    copy->object_type = le16_to_cpu(entry->object_type);
    copy->name_len = name_len;
    memcpy(copy->name, name, name_len);
    list_add_tail(&copy->link, &snapshot->entries);
    return 0;
}

static void infilfs_dir_snapshot_destroy(struct infilfs_dir_snapshot *snapshot)
{
    struct infilfs_dir_snapshot_entry *entry;
    struct infilfs_dir_snapshot_entry *next;

    list_for_each_entry_safe(entry, next, &snapshot->entries, link) {
        list_del(&entry->link);
        kfree(entry);
    }
}

static int infilfs_emit_snapshot_entry(
    const struct infilfs_dir_snapshot_entry *entry,
    struct infilfs_dir_emit_state *state)
{
    u16 name_len = entry->name_len;
    u16 type = entry->object_type;
    unsigned char dtype = infilfs_dtype(type);

    if (state->hide_linux_meta &&
        name_len == sizeof(INFILFS_LINUX_META_DIRECTORY) - 1u &&
        memcmp(entry->name, INFILFS_LINUX_META_DIRECTORY, name_len) == 0)
        return 0;

    if (type == INFILFS_OBJECT_FILE && state->has_linux_meta) {
        umode_t special = 0;

        if (!infilfs_linux_meta_get_special(state->dir->i_sb,
                                             entry->object_id,
                                             &special, NULL) && special) {
            if (S_ISFIFO(special))
                dtype = DT_FIFO;
            else if (S_ISSOCK(special))
                dtype = DT_SOCK;
            else if (S_ISCHR(special))
                dtype = DT_CHR;
            else if (S_ISBLK(special))
                dtype = DT_BLK;
        }
    }

    if (state->index < state->ctx->pos - 2) {
        state->index++;
        return 0;
    }

    if (!dir_emit(state->ctx, entry->name, name_len,
                  infilfs_object_ino(entry->object_id), dtype))
        return 1;
    state->index++;
    state->ctx->pos++;
    return 0;
}

static int infilfs_map_file_block(struct inode *inode, const u8 *object,
                                  u64 logical, u64 *physical_out,
                                  u32 *flags_out)
{
    const struct infilfs_object_header_disk *header =
        (const struct infilfs_object_header_disk *)object;
    const struct infilfs_file_payload_disk *file =
        (const struct infilfs_file_payload_disk *)(header + 1);
    u16 version = le16_to_cpu(header->object_version);
    u32 extent_count = le32_to_cpu(file->extent_count);
    u32 i;

    if (version == INFILFS_OBJECT_VERSION_CLASSIC) {
        const struct infilfs_extent_disk *ext =
            (const struct infilfs_extent_disk *)(file + 1);
        size_t needed = sizeof(*file) +
            (size_t)extent_count * sizeof(struct infilfs_extent_disk);

        if (needed > le32_to_cpu(header->payload_size))
            return -EFSCORRUPTED;
        for (i = 0; i < extent_count; ++i) {
            u64 start = le64_to_cpu(ext[i].logical_block);
            u32 blocks = le32_to_cpu(ext[i].block_count);
            u32 flags = le32_to_cpu(ext[i].flags);

            if (!blocks || logical < start || logical >= start + blocks)
                continue;
            *flags_out = flags;
            *physical_out = flags == INFILFS_EXTENT_HOLE ? 0 :
                le64_to_cpu(ext[i].physical_block) + (logical - start);
            return 0;
        }
        return -EFSCORRUPTED;
    }

    if (version == INFILFS_OBJECT_VERSION_PAGED) {
        const struct infilfs_extent_head_disk *head =
            (const struct infilfs_extent_head_disk *)(file + 1);
        const __le64 *pages = (const __le64 *)(head + 1);
        u32 page_count = le32_to_cpu(head->page_count);
        u8 *page_block;
        u32 lo = 0, hi = page_count;
        int ret = -EFSCORRUPTED;

        if (page_count == 0 || page_count > INFILFS_EXTENT_PAGE_POINTERS ||
            le32_to_cpu(head->reserved) != 0 ||
            sizeof(*file) + sizeof(*head) +
                (size_t)page_count * sizeof(__le64) !=
                    le32_to_cpu(header->payload_size))
            return -EFSCORRUPTED;
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
        if (!page_block)
            return -ENOMEM;

        /*
         * Extent pages are stored in logical order.  The original native
         * mapper restarted at page zero for every 4 KiB data block, making a
         * sequential read of a fragmented file O(data_blocks * extent_pages).
         * Locate the one candidate page with a binary search instead.
         */
        while (lo < hi) {
            const struct infilfs_metadata_page_disk *page;
            const struct infilfs_extent_disk *ext;
            u32 count;
            u32 mid = lo + (hi - lo) / 2u;
            u64 first;
            u64 last;

            ret = infilfs_read_allocated_block(
                inode->i_sb, le64_to_cpu(pages[mid]), page_block);
            if (ret)
                break;
            if (!infilfs_metadata_page_valid(inode->i_sb, page_block,
                                             infilfs_extent_page_magic,
                                             header->object_id)) {
                ret = -EFSCORRUPTED;
                break;
            }
            page = (const struct infilfs_metadata_page_disk *)page_block;
            count = le32_to_cpu(page->entry_count);
            if (count == 0 || count > INFILFS_EXTENTS_PER_PAGE ||
                le32_to_cpu(page->bytes_used) !=
                    count * sizeof(struct infilfs_extent_disk)) {
                ret = -EFSCORRUPTED;
                break;
            }
            ext = (const struct infilfs_extent_disk *)(page + 1);
            first = le64_to_cpu(ext[0].logical_block);
            last = le64_to_cpu(ext[count - 1u].logical_block) +
                le32_to_cpu(ext[count - 1u].block_count);
            if (last <= first) {
                ret = -EFSCORRUPTED;
                break;
            }
            if (logical < first) {
                hi = mid;
                continue;
            }
            if (logical >= last) {
                lo = mid + 1u;
                continue;
            }

            /* The target is inside this page; binary-search its extents. */
            {
                u32 elo = 0, ehi = count;

                while (elo < ehi) {
                    u32 emid = elo + (ehi - elo) / 2u;
                    u64 start = le64_to_cpu(ext[emid].logical_block);
                    u32 blocks = le32_to_cpu(ext[emid].block_count);
                    u32 flags = le32_to_cpu(ext[emid].flags);
                    u64 end_logical;

                    if (!blocks || start > U64_MAX - blocks) {
                        ret = -EFSCORRUPTED;
                        goto paged_out;
                    }
                    end_logical = start + blocks;
                    if (logical < start) {
                        ehi = emid;
                        continue;
                    }
                    if (logical >= end_logical) {
                        elo = emid + 1u;
                        continue;
                    }
                    if (flags != INFILFS_EXTENT_NORMAL &&
                        flags != INFILFS_EXTENT_HOLE) {
                        ret = -EFSCORRUPTED;
                        goto paged_out;
                    }
                    *flags_out = flags;
                    *physical_out = flags == INFILFS_EXTENT_HOLE ? 0 :
                        le64_to_cpu(ext[emid].physical_block) +
                            (logical - start);
                    ret = 0;
                    goto paged_out;
                }
            }
            ret = -EFSCORRUPTED;
            break;
        }
paged_out:
        kfree(page_block);
        return ret;
    }

    return -EFSCORRUPTED;
}

static __maybe_unused ssize_t infilfs_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct file *filep = iocb->ki_filp;
    struct inode *inode = file_inode(filep);
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    const struct infilfs_object_header_disk *header;
    const struct infilfs_file_payload_disk *file;
    loff_t pos = iocb->ki_pos;
    u64 file_size;
    size_t requested;
    size_t done = 0;
    u8 *object;
    u8 *data;
    int ret;

    if (pos < 0)
        return -EINVAL;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    data = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!object || !data) {
        ret = -ENOMEM;
        goto out;
    }

    ret = infilfs_read_object(inode->i_sb, ii->object_block,
                              INFILFS_OBJECT_FILE, ii->object_id, object);
    if (ret)
        goto out;
    header = (const struct infilfs_object_header_disk *)object;
    file = (const struct infilfs_file_payload_disk *)(header + 1);
    file_size = le64_to_cpu(file->attributes.logical_size);
    if ((u64)pos >= file_size) {
        ret = 0;
        goto out;
    }

    requested = min_t(u64, iov_iter_count(to), file_size - (u64)pos);

    if (le32_to_cpu(file->extent_count) == 0 &&
        file_size <= INFILFS_INLINE_DATA_MAX) {
        const u8 *inline_bytes = (const u8 *)(file + 1) +
            sizeof(struct infilfs_data_checksum_disk);
        size_t copied = copy_to_iter(inline_bytes + pos, requested, to);

        iocb->ki_pos += copied;
        ret = copied;
        goto out;
    }

    while (done < requested) {
        u64 absolute = (u64)pos + done;
        u64 logical = absolute >> INFILFS_DISK_BLOCK_SHIFT;
        size_t within = absolute & (INFILFS_DISK_BLOCK_SIZE - 1u);
        size_t chunk = min_t(size_t, INFILFS_DISK_BLOCK_SIZE - within,
                             requested - done);
        u64 physical = 0;
        u32 flags = 0;
        size_t copied;

        ret = infilfs_map_file_block(inode, object, logical,
                                     &physical, &flags);
        if (ret)
            goto out_partial;
        if (flags == INFILFS_EXTENT_HOLE) {
            memset(data, 0, INFILFS_DISK_BLOCK_SIZE);
        } else if (flags == INFILFS_EXTENT_NORMAL) {
            ret = infilfs_read_allocated_block(inode->i_sb, physical, data);
            if (ret)
                goto out_partial;
        } else {
            ret = -EFSCORRUPTED;
            goto out_partial;
        }

        copied = copy_to_iter(data + within, chunk, to);
        done += copied;
        if (copied != chunk)
            break;
    }

    iocb->ki_pos += done;
    ret = done;
    goto out;

out_partial:
    if (done) {
        iocb->ki_pos += done;
        ret = done;
    }
out:
    kfree(data);
    kfree(object);
    return ret;
}

static const char *infilfs_get_link(struct dentry *dentry, struct inode *inode,
                                    struct delayed_call *done)
{
    struct infilfs_inode_info *ii;

    (void)dentry;
    (void)done;
    if (!inode)
        return ERR_PTR(-ECHILD);
    ii = INFILFS_I(inode);
    if (!ii || !ii->symlink_target)
        return ERR_PTR(-EIO);
    return ii->symlink_target;
}

static int infilfs_file_allocated_blocks(
    struct inode *inode, const u8 object[INFILFS_DISK_BLOCK_SIZE],
    u64 *allocated_out)
{
    const struct infilfs_object_header_disk *header =
        (const struct infilfs_object_header_disk *)object;
    const struct infilfs_file_payload_disk *file =
        (const struct infilfs_file_payload_disk *)(header + 1);
    u32 extent_count = le32_to_cpu(file->extent_count);
    u64 allocated = 0;
    u32 copied = 0;
    int ret = 0;

    if (!extent_count) {
        *allocated_out = 0;
        return 0;
    }
    if (le16_to_cpu(header->object_version) == INFILFS_OBJECT_VERSION_CLASSIC) {
        const struct infilfs_extent_disk *extents =
            (const struct infilfs_extent_disk *)(file + 1);
        u32 i;

        if (sizeof(*file) + (size_t)extent_count * sizeof(*extents) !=
            le32_to_cpu(header->payload_size))
            return -EFSCORRUPTED;
        for (i = 0; i < extent_count; ++i)
            if (le32_to_cpu(extents[i].flags) == INFILFS_EXTENT_NORMAL)
                allocated += le32_to_cpu(extents[i].block_count);
    } else if (le16_to_cpu(header->object_version) ==
               INFILFS_OBJECT_VERSION_PAGED) {
        const struct infilfs_extent_head_disk *head =
            (const struct infilfs_extent_head_disk *)(file + 1);
        const __le64 *pages = (const __le64 *)(head + 1);
        u32 page_count = le32_to_cpu(head->page_count);
        u8 *page_block;
        u32 p;

        if (!page_count || page_count > INFILFS_EXTENT_PAGE_POINTERS ||
            le32_to_cpu(head->reserved) != 0)
            return -EFSCORRUPTED;
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
        if (!page_block)
            return -ENOMEM;
        for (p = 0; p < page_count; ++p) {
            const struct infilfs_metadata_page_disk *page;
            const struct infilfs_extent_disk *extents;
            u32 count;
            u32 i;

            ret = infilfs_read_allocated_block(
                inode->i_sb, le64_to_cpu(pages[p]), page_block);
            if (ret)
                break;
            if (!infilfs_metadata_page_valid(inode->i_sb, page_block,
                                             infilfs_extent_page_magic,
                                             header->object_id)) {
                ret = -EFSCORRUPTED;
                break;
            }
            page = (const struct infilfs_metadata_page_disk *)page_block;
            count = le32_to_cpu(page->entry_count);
            if (!count || count > INFILFS_EXTENTS_PER_PAGE ||
                copied > extent_count || count > extent_count - copied ||
                le32_to_cpu(page->bytes_used) !=
                    count * sizeof(*extents)) {
                ret = -EFSCORRUPTED;
                break;
            }
            extents = (const struct infilfs_extent_disk *)(page + 1);
            for (i = 0; i < count; ++i)
                if (le32_to_cpu(extents[i].flags) == INFILFS_EXTENT_NORMAL)
                    allocated += le32_to_cpu(extents[i].block_count);
            copied += count;
        }
        kfree(page_block);
        if (ret)
            return ret;
        if (copied != extent_count)
            return -EFSCORRUPTED;
    } else {
        return -EFSCORRUPTED;
    }
    *allocated_out = allocated;
    return 0;
}

static int infilfs_tree_directory_allocated_node(
    struct super_block *sb, const u8 owner_id[16], u64 node,
    unsigned int depth, u64 *blocks,
    struct infilfs_visit_set *visited)
{
    const struct infilfs_metadata_page_disk *page;
    const __le64 *children;
    u8 *raw;
    u32 expected;
    u32 seen = 0;
    u32 slot;
    int ret;

    if (!node || depth > INFILFS_DIRECTORY_TREE_DEPTH ||
        !infilfs_block_allocated(sb, node))
        return -EFSCORRUPTED;
    ret = infilfs_visit_claim(visited, node);
    if (ret)
        return ret;
    raw = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!raw)
        return -ENOMEM;
    ret = infilfs_read_allocated_block(sb, node, raw);
    if (ret)
        goto out;

    if (!memcmp(raw, infilfs_directory_page_magic, 8)) {
        if (!infilfs_metadata_page_valid(
                sb, raw, infilfs_directory_page_magic, owner_id)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        if (*blocks == U64_MAX) {
            ret = -EOVERFLOW;
            goto out;
        }
        (*blocks)++;
        ret = 0;
        goto out;
    }

    if (depth >= INFILFS_DIRECTORY_TREE_DEPTH ||
        !infilfs_metadata_page_valid(
            sb, raw, infilfs_directory_branch_page_magic, owner_id)) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    page = (const struct infilfs_metadata_page_disk *)raw;
    expected = le32_to_cpu(page->entry_count);
    if (!expected || expected > INFILFS_DIRECTORY_TREE_FANOUT ||
        le32_to_cpu(page->bytes_used) !=
            INFILFS_DIRECTORY_TREE_BRANCH_BYTES) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    if (*blocks == U64_MAX) {
        ret = -EOVERFLOW;
        goto out;
    }
    (*blocks)++;
    children = (const __le64 *)(page + 1);
    for (slot = 0; slot < INFILFS_DIRECTORY_TREE_FANOUT; ++slot) {
        u64 child = le64_to_cpu(children[slot]);

        if (!child)
            continue;
        seen++;
        ret = infilfs_tree_directory_allocated_node(
            sb, owner_id, child, depth + 1u, blocks, visited);
        if (ret)
            goto out;
    }
    ret = seen == expected ? 0 : -EFSCORRUPTED;
out:
    kfree(raw);
    return ret;
}

static int infilfs_metadata_allocated_blocks(
    struct super_block *sb,
    const u8 object[INFILFS_DISK_BLOCK_SIZE], u64 *allocated_out)
{
    const struct infilfs_object_header_disk *header =
        (const struct infilfs_object_header_disk *)object;
    u16 type = le16_to_cpu(header->object_type);
    u16 version = le16_to_cpu(header->object_version);

    if (type == INFILFS_OBJECT_SYMLINK) {
        *allocated_out = 1;
        return 0;
    }
    if (type != INFILFS_OBJECT_DIRECTORY)
        return -EINVAL;
    if (version == INFILFS_OBJECT_VERSION_CLASSIC) {
        *allocated_out = 1;
        return 0;
    }
    if (version == INFILFS_OBJECT_VERSION_PAGED) {
        const struct infilfs_directory_payload_disk *payload =
            (const struct infilfs_directory_payload_disk *)(header + 1);
        u32 page_count = le32_to_cpu(payload->bytes_used);
        size_t needed = sizeof(*payload) +
            (size_t)page_count * sizeof(__le64);

        if (page_count > INFILFS_DIRECTORY_PAGE_POINTERS ||
            needed > le32_to_cpu(header->payload_size))
            return -EFSCORRUPTED;
        *allocated_out = 1u + page_count;
        return 0;
    }
    if (version == INFILFS_OBJECT_VERSION_TREE) {
        const struct infilfs_directory_payload_disk *payload =
            (const struct infilfs_directory_payload_disk *)(header + 1);
        __le64 root_le;
        u64 root;
        u64 blocks = 1;
        struct infilfs_visit_set visited = {0};
        u32 entries = le32_to_cpu(payload->entry_count);
        int ret;

        if (le32_to_cpu(payload->bytes_used) != 0 ||
            le32_to_cpu(header->payload_size) !=
                sizeof(*payload) + sizeof(root_le))
            return -EFSCORRUPTED;
        memcpy(&root_le, payload + 1, sizeof(root_le));
        root = le64_to_cpu(root_le);
        if (!entries)
            return root ? -EFSCORRUPTED : (*allocated_out = 1, 0);
        if (!root)
            return -EFSCORRUPTED;
        ret = infilfs_tree_directory_allocated_node(
            sb, header->object_id, root, 0u, &blocks, &visited);
        infilfs_visit_destroy(&visited);
        if (ret)
            return ret;
        *allocated_out = blocks;
        return 0;
    }
    return -EFSCORRUPTED;
}

static int infilfs_refresh_inode_blocks(struct inode *inode)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    u8 *object;
    u64 allocated;
    int ret;

    if (!ii || (ii->object_type != INFILFS_OBJECT_DIRECTORY &&
                ii->object_type != INFILFS_OBJECT_SYMLINK))
        return 0;
    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;
    ret = infilfs_read_object(inode->i_sb, ii->object_block,
                              ii->object_type, ii->object_id, object);
    if (!ret)
        ret = infilfs_metadata_allocated_blocks(inode->i_sb, object, &allocated);
    if (!ret)
        inode->i_blocks = allocated * (INFILFS_DISK_BLOCK_SIZE >> 9);
    kfree(object);
    return ret;
}

static void infilfs_refresh_inode_blocks_after_commit(struct inode *inode)
{
    int ret = infilfs_refresh_inode_blocks(inode);

    if (ret)
        pr_err("InfiltratorFS: could not refresh allocation accounting for inode %lu: %d\n",
               inode->i_ino, ret);
}

static int infilfs_populate_inode(struct inode *inode, u64 object_block,
                                  u16 expected_type, const u8 expected_id[16])
{
    struct infilfs_inode_info *ii;
    const struct infilfs_object_header_disk *header;
    const struct infilfs_attributes_disk *attributes;
    const struct infilfs_posix_compat_disk *posix;
    u8 *object;
    umode_t permissions;
    u64 links;
    int ret;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!object)
        return -ENOMEM;
    ret = infilfs_read_object(inode->i_sb, object_block, expected_type,
                              expected_id, object);
    if (ret)
        goto out;

    header = (const struct infilfs_object_header_disk *)object;
    ii = kzalloc(sizeof(*ii), GFP_KERNEL);
    if (!ii) {
        ret = -ENOMEM;
        goto out;
    }
    ii->object_block = object_block;
    ii->object_type = le16_to_cpu(header->object_type);
    memcpy(ii->object_id, header->object_id, 16);
    inode->i_private = ii;

    if (ii->object_type == INFILFS_OBJECT_DIRECTORY) {
        const struct infilfs_directory_payload_disk *payload =
            (const struct infilfs_directory_payload_disk *)(header + 1);
        attributes = &payload->attributes;
        posix = &payload->posix;
        permissions = le32_to_cpu(posix->permissions) & 07777;
        inode->i_mode = S_IFDIR | permissions;
        inode->i_op = &infilfs_dir_inode_operations;
        inode->i_fop = &infilfs_dir_operations;
        links = le64_to_cpu(attributes->link_count);
        set_nlink(inode, links >= 2 ? links : 2);
        i_size_write(inode, le64_to_cpu(attributes->logical_size));
        ret = infilfs_metadata_allocated_blocks(inode->i_sb, object, &links);
        if (ret)
            goto fail_private;
        inode->i_blocks = links * (INFILFS_DISK_BLOCK_SIZE >> 9);
    } else if (ii->object_type == INFILFS_OBJECT_FILE) {
        const struct infilfs_file_payload_disk *payload =
            (const struct infilfs_file_payload_disk *)(header + 1);
        umode_t special_mode = 0;
        dev_t special_rdev = 0;

        attributes = &payload->attributes;
        posix = &payload->posix;
        permissions = le32_to_cpu(posix->permissions) & 07777;
        inode->i_op = &infilfs_file_inode_operations;
        links = le64_to_cpu(attributes->link_count);
        set_nlink(inode, links ? links : 1);
        ret = infilfs_linux_meta_get_special(inode->i_sb, ii->object_id,
                                              &special_mode, &special_rdev);
        if (ret)
            goto fail_private;
        if (special_mode) {
            if (!S_ISFIFO(special_mode) && !S_ISSOCK(special_mode) &&
                !S_ISCHR(special_mode) && !S_ISBLK(special_mode)) {
                ret = -EFSCORRUPTED;
                goto fail_private;
            }
            if (le64_to_cpu(attributes->logical_size) != 0 ||
                le32_to_cpu(payload->extent_count) != 0) {
                ret = -EFSCORRUPTED;
                goto fail_private;
            }
            init_special_inode(inode, special_mode | permissions,
                               special_rdev);
            i_size_write(inode, 0);
            inode->i_blocks = 0;
        } else {
            u64 allocated;

            inode->i_mode = S_IFREG | permissions;
            inode->i_fop = &infilfs_file_operations;
            inode->i_mapping->a_ops = &infilfs_aops;
            i_size_write(inode, le64_to_cpu(attributes->logical_size));
            ret = infilfs_file_allocated_blocks(inode, object, &allocated);
            if (ret)
                goto fail_private;
            inode->i_blocks = allocated *
                (INFILFS_DISK_BLOCK_SIZE >> 9);
        }
    } else if (ii->object_type == INFILFS_OBJECT_SYMLINK) {
        const struct infilfs_symlink_payload_disk *payload =
            (const struct infilfs_symlink_payload_disk *)(header + 1);
        u32 target_len = le32_to_cpu(payload->target_length);
        const u8 *target = (const u8 *)(payload + 1);

        attributes = &payload->attributes;
        posix = &payload->posix;
        if (target_len == 0 || target_len > INFILFS_DISK_BLOCK_SIZE -
            sizeof(*header) - sizeof(*payload) ||
            sizeof(*payload) + target_len > le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto fail_private;
        }
        ii->symlink_target = kmemdup_nul(target, target_len, GFP_KERNEL);
        if (!ii->symlink_target) {
            ret = -ENOMEM;
            goto fail_private;
        }
        inode->i_mode = S_IFLNK | 0777;
        inode->i_op = &infilfs_symlink_inode_operations;
        set_nlink(inode, 1);
        i_size_write(inode, target_len);
        inode->i_blocks = INFILFS_DISK_BLOCK_SIZE >> 9;
    } else {
        ret = -EOPNOTSUPP;
        goto fail_private;
    }

    {
        kuid_t uid = make_kuid(&init_user_ns, le32_to_cpu(posix->uid));
        kgid_t gid = make_kgid(&init_user_ns, le32_to_cpu(posix->gid));
        inode->i_uid = uid_valid(uid) ? uid : GLOBAL_ROOT_UID;
        inode->i_gid = gid_valid(gid) ? gid : GLOBAL_ROOT_GID;
    }
    ret = 0;
    goto out;

fail_private:
    kfree(ii->symlink_target);
    kfree(ii);
    inode->i_private = NULL;
out:
    kfree(object);
    return ret;
}

static struct inode *infilfs_get_inode(struct super_block *sb, u64 object_block,
                                       u16 expected_type,
                                       const u8 expected_id[16])
{
    struct inode *inode;
    u64 ino = infilfs_object_ino(expected_id);
    int ret;

    inode = iget_locked(sb, ino);
    if (!inode)
        return ERR_PTR(-ENOMEM);
    if (!infilfs_inode_is_new(inode))
        return inode;

    ret = infilfs_populate_inode(inode, object_block, expected_type, expected_id);
    if (ret) {
        iget_failed(inode);
        return ERR_PTR(ret);
    }
    unlock_new_inode(inode);
    return inode;
}

static struct dentry *infilfs_lookup(struct inode *dir, struct dentry *dentry,
                                     unsigned int flags)
{
    struct infilfs_dir_lookup search = {
        .name = dentry->d_name.name,
        .name_len = dentry->d_name.len,
    };
    struct inode *inode;
    u64 object_block;
    u16 indexed_type;
    int ret;

    (void)flags;
    if (search.name_len == 0 || search.name_len > INFILFS_NAME_MAX)
        return ERR_PTR(-ENAMETOOLONG);
    if (dir == d_inode(dir->i_sb->s_root) &&
        search.name_len == sizeof(INFILFS_LINUX_META_DIRECTORY) - 1u &&
        memcmp(search.name, INFILFS_LINUX_META_DIRECTORY,
               search.name_len) == 0 &&
        infilfs_linux_meta_directory_is_internal(dir->i_sb)) {
        d_add(dentry, NULL);
        return NULL;
    }

    /*
     * Deferred writers advance directory/index roots and later reclaim the
     * superseded metadata at publication.  A lookup must not start from an
     * older root and then have one of its child blocks reclaimed underneath
     * it.  Serialize only the persistent topology walk with the native writer;
     * drop the lock before inode instantiation because that may consult Linux
     * sidecar metadata and re-enter read-only namespace helpers.
     */
    mutex_lock(&INFILFS_SB(dir->i_sb)->write_lock);
    ret = infilfs_tree_dir_lookup_name(
        dir, search.name, (u16)search.name_len, &search);
    if (ret == -EOPNOTSUPP)
        ret = infilfs_for_each_dirent(dir, infilfs_lookup_visitor, &search);
    if (ret < 0) {
        if (ret == -EFSCORRUPTED)
            pr_err("InfiltratorFS: lookup directory traversal saw corrupt metadata (dir_block=%llu generation=%llu name_len=%u)\n",
                   (unsigned long long)INFILFS_I(dir)->object_block,
                   (unsigned long long)le64_to_cpu(
                       INFILFS_SB(dir->i_sb)->disk.generation),
                   search.name_len);
        mutex_unlock(&INFILFS_SB(dir->i_sb)->write_lock);
        return ERR_PTR(ret);
    }
    if (!search.found) {
        mutex_unlock(&INFILFS_SB(dir->i_sb)->write_lock);
        d_add(dentry, NULL);
        return NULL;
    }

    ret = infilfs_index_lookup(dir->i_sb, search.object_id,
                               &object_block, &indexed_type);
    if (ret) {
        if (ret == -EFSCORRUPTED)
            pr_err("InfiltratorFS: lookup object-index traversal saw corrupt metadata (generation=%llu)\n",
                   (unsigned long long)le64_to_cpu(
                       INFILFS_SB(dir->i_sb)->disk.generation));
        mutex_unlock(&INFILFS_SB(dir->i_sb)->write_lock);
        return ERR_PTR(ret);
    }
    if (indexed_type != search.object_type) {
        mutex_unlock(&INFILFS_SB(dir->i_sb)->write_lock);
        return ERR_PTR(-EFSCORRUPTED);
    }
    mutex_unlock(&INFILFS_SB(dir->i_sb)->write_lock);

    inode = infilfs_get_inode(dir->i_sb, object_block, indexed_type,
                              search.object_id);
    if (IS_ERR(inode))
        return ERR_CAST(inode);
    return d_splice_alias(inode, dentry);
}

static int infilfs_iterate_shared(struct file *file, struct dir_context *ctx)
{
    bool has_linux_meta =
        infilfs_linux_meta_directory_is_internal(file_inode(file)->i_sb);
    struct infilfs_dir_emit_state state = {
        .ctx = ctx,
        .dir = file_inode(file),
        .has_linux_meta = has_linux_meta,
        .hide_linux_meta = file_inode(file) ==
            d_inode(file_inode(file)->i_sb->s_root) &&
            has_linux_meta,
        .index = 0,
    };
    struct infilfs_dir_snapshot snapshot;
    struct infilfs_dir_snapshot_entry *entry;
    int ret;

    if (!dir_emit_dots(file, ctx))
        return 0;

    INIT_LIST_HEAD(&snapshot.entries);

    /*
     * Like lookup, readdir can span several metadata blocks.  Snapshot the
     * persistent directory entries while holding the writer lock so deferred
     * publication cannot reclaim a child block during the topology walk.
     *
     * Do not keep write_lock while resolving Linux sidecar metadata for d_type:
     * that resolution performs its own protected sidecar/index lookup and would
     * recursively acquire write_lock.  Emitting from copied entries after the
     * lock is released preserves both topology safety and lock ordering.
     */
    mutex_lock(&INFILFS_SB(file_inode(file)->i_sb)->write_lock);
    ret = infilfs_for_each_dirent(file_inode(file),
                                  infilfs_snapshot_visitor, &snapshot);
    mutex_unlock(&INFILFS_SB(file_inode(file)->i_sb)->write_lock);
    if (ret < 0)
        goto out;

    list_for_each_entry(entry, &snapshot.entries, link) {
        if (infilfs_emit_snapshot_entry(entry, &state))
            break;
    }
    ret = 0;
out:
    infilfs_dir_snapshot_destroy(&snapshot);
    return ret;
}

#include "infiltratorfs_rw.inc"
#include "infiltratorfs_defrag.inc"

static void infilfs_evict_inode(struct inode *inode)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    int ret;

    truncate_inode_pages_final(&inode->i_data);
    ret = 0;
    if (ii && inode->i_nlink == 0 && !sb_rdonly(inode->i_sb)) {
        ret = infilfs_linux_meta_remove_object(inode->i_sb, ii->object_id);
        if (ret)
            pr_err("InfiltratorFS: could not remove Linux metadata for inode %lu: %d\n",
                   inode->i_ino, ret);
    }
    if (!ret)
        ret = infilfs_ns_evict_unlinked_file(inode);
    if (ret)
        pr_err("InfiltratorFS: could not reclaim unlinked inode %lu: %d\n",
               inode->i_ino, ret);
    clear_inode(inode);
    if (ii) {
        kfree(ii->symlink_target);
        kfree(ii);
        inode->i_private = NULL;
    }
}

static void infilfs_put_super(struct super_block *sb)
{
    infilfs_rw_mount_destroy(sb);
    kfree(sb->s_fs_info);
    sb->s_fs_info = NULL;
}

static int infilfs_show_options(struct seq_file *seq, struct dentry *root)
{
    const struct infilfs_sb_info *sbi = INFILFS_SB(root->d_sb);

    if (sbi)
        seq_printf(seq, ",media=%s", infilfs_media_profile_name(
            sbi->media_profile));
    return 0;
}

static const struct super_operations infilfs_super_operations = {
    .statfs = simple_statfs,
    .sync_fs = infilfs_sync_fs,
    .evict_inode = infilfs_evict_inode,
    .put_super = infilfs_put_super,
    .show_options = infilfs_show_options,
};

static const struct inode_operations infilfs_dir_inode_operations = {
    .lookup = infilfs_lookup,
    .create = infilfs_rw_create,
    .mkdir = infilfs_rw_mkdir,
    .listxattr = infilfs_linux_listxattr,
};

static const struct inode_operations infilfs_file_inode_operations = {
    .setattr = infilfs_rw_setattr,
    .listxattr = infilfs_linux_listxattr,
};

static const struct inode_operations infilfs_symlink_inode_operations = {
    .get_link = infilfs_get_link,
    .listxattr = infilfs_linux_listxattr,
};

static const struct file_operations infilfs_dir_operations = {
    .owner = THIS_MODULE,
    .llseek = generic_file_llseek,
    .iterate_shared = infilfs_iterate_shared,
    .fsync = infilfs_file_fsync,
};

static const struct file_operations infilfs_file_operations = {
    .owner = THIS_MODULE,
    .llseek = generic_file_llseek,
    .read_iter = infilfs_file_read_iter,
    .write_iter = infilfs_file_write_iter,
    .mmap = generic_file_mmap,
    .fallocate = infilfs_file_fallocate,
    .unlocked_ioctl = infilfs_file_ioctl,
    .fsync = infilfs_file_fsync,
};

static int infilfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
    struct infilfs_sb_info *sbi;
    struct inode *root_inode;
    u64 bytes;
    const struct infilfs_fs_context *ctx =
        fc ? fc->fs_private : NULL;
    int ret;

    if (!sb_set_blocksize(sb, INFILFS_DISK_BLOCK_SIZE))
        return -EINVAL;

    bytes = bdev_nr_bytes(sb->s_bdev);
    if (bytes < INFILFS_DISK_BLOCK_SIZE * 3ULL ||
        (bytes & (INFILFS_DISK_BLOCK_SIZE - 1u)) != 0)
        return -EINVAL;

    sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
    if (!sbi)
        return -ENOMEM;
    sbi->device_blocks = bytes >> INFILFS_DISK_BLOCK_SHIFT;
    ret = infilfs_resolve_media_profile(sb, ctx, sbi);
    if (ret) {
        kfree(sbi);
        return ret;
    }
    mutex_init(&sbi->write_lock);
    mutex_init(&sbi->linux_meta_lock);
    rwlock_init(&sbi->bitmap_lock);
    sb->s_fs_info = sbi;

    ret = infilfs_select_checkpoint(sb, &sbi->disk);
    if (ret)
        goto fail;
    ret = infilfs_rw_mount_init(sb);
    if (ret)
        goto fail;
    ret = infilfs_rw_heal_checkpoints(sb);
    if (ret)
        goto fail;

    sb->s_magic = INFILTRATORFS_MAGIC;
    sb->s_maxbytes = MAX_LFS_FILESIZE;
    sb->s_time_gran = 1;
    sb->s_op = &infilfs_super_operations;
    sb->s_xattr = infilfs_xattr_handlers;

    root_inode = infilfs_get_inode(sb, le64_to_cpu(sbi->disk.root_object_block),
                                   INFILFS_OBJECT_DIRECTORY,
                                   sbi->disk.root_object_id);
    if (IS_ERR(root_inode)) {
        ret = PTR_ERR(root_inode);
        goto fail;
    }

    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root) {
        ret = -ENOMEM;
        goto fail;
    }
    ret = infilfs_native_recover_unlinked_files(sb);
    if (ret)
        goto fail;

    pr_info("InfiltratorFS: native %s mount Format %u.%u generation %llu media=%s media_source=%s\n",
            sb_rdonly(sb) ? "read-only" : "read-write",
            INFILFS_FORMAT_MAJOR, INFILFS_FORMAT_MINOR,
            (unsigned long long)le64_to_cpu(sbi->disk.generation),
            infilfs_media_profile_name(sbi->media_profile),
            sbi->media_profile_overridden ? "override" : "auto");
    return 0;

fail:
    if (sb->s_root) {
        dput(sb->s_root);
        sb->s_root = NULL;
    }
    infilfs_rw_mount_destroy(sb);
    kfree(sbi);
    sb->s_fs_info = NULL;
    return ret;
}

enum {
    Opt_media,
};

static const struct constant_table infilfs_media_param_values[] = {
    { "auto", INFILFS_MEDIA_OVERRIDE_AUTO },
    { "balanced", INFILFS_MEDIA_OVERRIDE_BALANCED },
    { "rotational", INFILFS_MEDIA_OVERRIDE_ROTATIONAL },
    { "nonrotational", INFILFS_MEDIA_OVERRIDE_NONROTATIONAL },
    {}
};

static const struct fs_parameter_spec infilfs_fs_parameters[] = {
    fsparam_enum("media", Opt_media, infilfs_media_param_values),
    {}
};

static int infilfs_parse_param(
    struct fs_context *fc, struct fs_parameter *param)
{
    struct infilfs_fs_context *ctx = fc->fs_private;
    struct fs_parse_result result;
    int opt;
    int ret;

    ret = vfs_parse_fs_param_source(fc, param);
    if (ret != -ENOPARAM)
        return ret;
    opt = fs_parse(fc, infilfs_fs_parameters, param, &result);
    if (opt < 0)
        return opt;
    switch (opt) {
    case Opt_media:
        ctx->media_override =
            (enum infilfs_media_override)result.uint_32;
        return 0;
    default:
        return -EINVAL;
    }
}

static void infilfs_free_fs_context(struct fs_context *fc)
{
    kfree(fc->fs_private);
    fc->fs_private = NULL;
}

static int infilfs_get_tree(struct fs_context *fc)
{
    return get_tree_bdev(fc, infilfs_fill_super);
}

static const struct fs_context_operations infilfs_context_operations = {
    .free = infilfs_free_fs_context,
    .parse_param = infilfs_parse_param,
    .get_tree = infilfs_get_tree,
};

static int infilfs_init_fs_context(struct fs_context *fc)
{
    struct infilfs_fs_context *ctx;

    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;
    ctx->media_override = INFILFS_MEDIA_OVERRIDE_AUTO;
    fc->fs_private = ctx;
    fc->ops = &infilfs_context_operations;
    return 0;
}

static struct file_system_type infilfs_type = {
    .owner = THIS_MODULE,
    .name = INFILTRATORFS_NAME,
    .parameters = infilfs_fs_parameters,
    .init_fs_context = infilfs_init_fs_context,
    .kill_sb = kill_block_super,
    .fs_flags = FS_REQUIRES_DEV,
};

static int __init infilfs_init(void)
{
    int status = register_filesystem(&infilfs_type);

    if (status == 0)
        pr_info("InfiltratorFS: native Linux VFS registered with read-write support\n");
    return status;
}

static void __exit infilfs_exit(void)
{
    unregister_filesystem(&infilfs_type);
    pr_info("InfiltratorFS: native Linux VFS unloaded\n");
}

module_init(infilfs_init);
module_exit(infilfs_exit);

MODULE_DESCRIPTION("InfiltratorFS native Linux VFS driver with read-write support");
MODULE_AUTHOR("The First Infiltrator");
MODULE_LICENSE("GPL");
MODULE_ALIAS_FS(INFILTRATORFS_NAME);
