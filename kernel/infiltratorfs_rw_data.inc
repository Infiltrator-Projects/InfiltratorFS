// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Native Linux extent-backed data path for Format 0.12.
 *
 * The original RW bring-up deliberately stopped at inline files.  This layer
 * keeps that proven implementation as the small-file path and adds a bounded,
 * deferred native transaction for extent-backed sequential and random writes.
 * Committed data blocks are never overwritten in place; replacement and
 * reclamation consult both live ownership and the retained-snapshot union.
 *
 * Continuous writers share one transaction per mounted superblock.  The
 * working object-index/root pointers are visible to the live mount while the
 * committed bitmap remains untouched.  Publication occurs at the same
 * adaptive 16 MiB..256 MiB bound used by the userspace adapter, on fsync, one
 * second after the writer becomes idle, or during unmount.  Transaction-
 * private metadata blocks are safely reused between syscalls; a small undo log
 * plus a bitmap snapshot gives each write syscall operation-level rollback.
 */

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define INFILFS_NATIVE_WRITE_CHUNK (4u * 1024u * 1024u)
#define INFILFS_NATIVE_IDLE_DELAY HZ
#define INFILFS_NATIVE_UNDO_MAX 16u
#define INFILFS_NATIVE_CHECKSUM_CACHE_SLOTS 256u

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

struct infilfs_native_index_change {
    u8 object_id[16];
    u64 object_block;
    u16 object_type;
    bool add;
    bool found;
};

struct infilfs_native_undo {
    u64 block;
    u8 data[INFILFS_DISK_BLOCK_SIZE];
};

struct infilfs_native_pending {
    struct list_head node;
    struct super_block *sb;
    struct infilfs_rw_tx tx;
    struct infilfs_superblock_disk base_disk;
    struct delayed_work idle_work;
    u8 *operation_bitmap;
    size_t operation_bitmap_bytes;
    struct infilfs_superblock_disk operation_next_sb;
    u64 operation_free_blocks;
    size_t operation_deferred_count;
    struct infilfs_rw_free_range operation_last_deferred;
    bool operation_had_last;
    struct infilfs_native_undo undo[INFILFS_NATIVE_UNDO_MAX];
    unsigned int undo_count;
    u64 pending_bytes;
    u64 publish_threshold;
    bool active;
    bool commit_failed;
};

struct infilfs_native_checksum_cache_entry {
    struct super_block *sb;
    u8 owner_id[16];
    u8 object_id[16];
    u64 object_block;
    u64 start_logical;
    bool valid;
};

struct infilfs_native_checksum_node {
    u8 object_id[16];
    u64 object_block;
    u64 start_logical;
    u32 checksum_count;
    bool new_node;
    bool dirty;
};

static LIST_HEAD(infilfs_native_pending_list);
static DEFINE_MUTEX(infilfs_native_pending_list_lock);
static DEFINE_SPINLOCK(infilfs_native_checksum_cache_lock);
static struct infilfs_native_checksum_cache_entry
    infilfs_native_checksum_cache[INFILFS_NATIVE_CHECKSUM_CACHE_SLOTS];

/* Implemented by the namespace layer after this data layer is composed. */
static int infilfs_ns_free_unshared_run(
    struct infilfs_native_pending *pending, const u8 owner_id[16],
    u64 start, u64 count);

static u32 infilfs_native_id_hash(const u8 id[16])
{
    u32 h = 2166136261u;
    unsigned int i;

    for (i = 0; i < 16; ++i) {
        h ^= id[i];
        h *= 16777619u;
    }
    return h;
}

static void infilfs_native_checksum_cache_invalidate_sb(struct super_block *sb)
{
    unsigned long flags;
    unsigned int i;

    spin_lock_irqsave(&infilfs_native_checksum_cache_lock, flags);
    for (i = 0; i < INFILFS_NATIVE_CHECKSUM_CACHE_SLOTS; ++i)
        if (infilfs_native_checksum_cache[i].valid &&
            infilfs_native_checksum_cache[i].sb == sb)
            infilfs_native_checksum_cache[i].valid = false;
    spin_unlock_irqrestore(&infilfs_native_checksum_cache_lock, flags);
}

static bool infilfs_native_checksum_cache_lookup(
    struct super_block *sb, const u8 owner_id[16],
    struct infilfs_native_checksum_cache_entry *out)
{
    unsigned long flags;
    u32 slot = infilfs_native_id_hash(owner_id) &
        (INFILFS_NATIVE_CHECKSUM_CACHE_SLOTS - 1u);
    bool found = false;

    spin_lock_irqsave(&infilfs_native_checksum_cache_lock, flags);
    if (infilfs_native_checksum_cache[slot].valid &&
        infilfs_native_checksum_cache[slot].sb == sb &&
        memcmp(infilfs_native_checksum_cache[slot].owner_id,
               owner_id, 16) == 0) {
        if (out)
            *out = infilfs_native_checksum_cache[slot];
        found = true;
    }
    spin_unlock_irqrestore(&infilfs_native_checksum_cache_lock, flags);
    return found;
}

static void infilfs_native_checksum_cache_store(
    struct super_block *sb, const u8 owner_id[16], const u8 object_id[16],
    u64 object_block, u64 start_logical)
{
    unsigned long flags;
    u32 slot = infilfs_native_id_hash(owner_id) &
        (INFILFS_NATIVE_CHECKSUM_CACHE_SLOTS - 1u);
    struct infilfs_native_checksum_cache_entry *entry;

    spin_lock_irqsave(&infilfs_native_checksum_cache_lock, flags);
    entry = &infilfs_native_checksum_cache[slot];
    memset(entry, 0, sizeof(*entry));
    entry->sb = sb;
    memcpy(entry->owner_id, owner_id, 16);
    memcpy(entry->object_id, object_id, 16);
    entry->object_block = object_block;
    entry->start_logical = start_logical;
    entry->valid = true;
    spin_unlock_irqrestore(&infilfs_native_checksum_cache_lock, flags);
}

static int infilfs_native_stage_block(struct super_block *sb, u64 block,
                                      const void *data)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct buffer_head *bh;

    if (!sbi || block >= sbi->device_blocks)
        return -EIO;
    bh = sb_getblk(sb, (sector_t)block);
    if (!bh)
        return -EIO;
    lock_buffer(bh);
    memcpy(bh->b_data, data, INFILFS_DISK_BLOCK_SIZE);
    set_buffer_uptodate(bh);
    mark_buffer_dirty(bh);
    unlock_buffer(bh);
    brelse(bh);
    return 0;
}

static bool infilfs_native_block_private(struct infilfs_native_pending *pending,
                                         u64 block)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(pending->sb);

    return sbi && block < sbi->device_blocks && sbi->bitmap &&
        !infilfs_rw_bitmap_get(sbi->bitmap, block) &&
        infilfs_rw_bitmap_get(pending->tx.bitmap, block);
}

static int infilfs_native_undo_capture(struct infilfs_native_pending *pending,
                                       u64 block)
{
    unsigned int i;
    int ret;

    for (i = 0; i < pending->undo_count; ++i)
        if (pending->undo[i].block == block)
            return 0;
    if (pending->undo_count >= INFILFS_NATIVE_UNDO_MAX)
        return -E2BIG;
    ret = infilfs_read_block(pending->sb, block,
                             pending->undo[pending->undo_count].data);
    if (ret)
        return ret;
    pending->undo[pending->undo_count].block = block;
    pending->undo_count++;
    return 0;
}

static int infilfs_native_store_private_or_cow(
    struct infilfs_native_pending *pending, u64 old_block,
    const u8 data[INFILFS_DISK_BLOCK_SIZE], u64 *new_block_out)
{
    int ret;
    u64 new_block;

    if (old_block && infilfs_native_block_private(pending, old_block)) {
        ret = infilfs_native_undo_capture(pending, old_block);
        if (ret)
            return ret;
        ret = infilfs_native_stage_block(pending->sb, old_block, data);
        if (!ret)
            *new_block_out = old_block;
        return ret;
    }

    ret = infilfs_rw_tx_alloc(&pending->tx, 1, &new_block);
    if (ret)
        return ret;
    ret = infilfs_native_stage_block(pending->sb, new_block, data);
    if (ret)
        return ret;
    if (old_block) {
        ret = infilfs_rw_tx_defer_free(&pending->tx, old_block, 1);
        if (ret)
            return ret;
    }
    *new_block_out = new_block;
    return 0;
}

static struct infilfs_native_pending *
infilfs_native_pending_find_locked(struct super_block *sb)
{
    struct infilfs_native_pending *pending;

    list_for_each_entry(pending, &infilfs_native_pending_list, node)
        if (pending->sb == sb)
            return pending;
    return NULL;
}

static void infilfs_native_idle_work(struct work_struct *work);

static struct infilfs_native_pending *
infilfs_native_pending_get(struct super_block *sb, bool create)
{
    struct infilfs_native_pending *pending;

    mutex_lock(&infilfs_native_pending_list_lock);
    pending = infilfs_native_pending_find_locked(sb);
    if (!pending && create) {
        struct infilfs_sb_info *sbi = INFILFS_SB(sb);
        u64 bytes = sbi ? sbi->device_blocks << INFILFS_DISK_BLOCK_SHIFT : 0;
        u64 threshold = bytes / 64u;
        const u64 min_publish = 16ULL * 1024ULL * 1024ULL;
        const u64 max_publish = 256ULL * 1024ULL * 1024ULL;

        if (threshold < min_publish)
            threshold = min_publish;
        if (threshold > max_publish)
            threshold = max_publish;
        pending = kzalloc(sizeof(*pending), GFP_KERNEL);
        if (pending) {
            pending->sb = sb;
            pending->publish_threshold = threshold;
            INIT_DELAYED_WORK(&pending->idle_work, infilfs_native_idle_work);
            list_add(&pending->node, &infilfs_native_pending_list);
        }
    }
    mutex_unlock(&infilfs_native_pending_list_lock);
    return pending;
}

static int infilfs_native_pending_begin_locked(
    struct infilfs_native_pending *pending)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(pending->sb);
    int ret;

    if (pending->commit_failed)
        return -EIO;
    if (pending->active)
        return 0;
    ret = infilfs_rw_tx_begin(pending->sb, &pending->tx);
    if (ret)
        return ret;
    pending->base_disk = sbi->disk;
    pending->pending_bytes = 0;
    pending->active = true;
    return 0;
}

static int infilfs_native_operation_snapshot(
    struct infilfs_native_pending *pending)
{
    size_t bytes = pending->tx.bitmap_bytes;

    if (pending->operation_bitmap_bytes != bytes) {
        kvfree(pending->operation_bitmap);
        pending->operation_bitmap = kvmalloc(bytes, GFP_NOFS);
        if (!pending->operation_bitmap) {
            pending->operation_bitmap_bytes = 0;
            return -ENOMEM;
        }
        pending->operation_bitmap_bytes = bytes;
    }
    memcpy(pending->operation_bitmap, pending->tx.bitmap, bytes);
    pending->operation_next_sb = pending->tx.next_sb;
    pending->operation_free_blocks = pending->tx.free_blocks;
    pending->operation_deferred_count = pending->tx.deferred_count;
    pending->operation_had_last = pending->tx.deferred_count != 0;
    if (pending->operation_had_last)
        pending->operation_last_deferred =
            pending->tx.deferred[pending->tx.deferred_count - 1u];
    pending->undo_count = 0;
    return 0;
}

static int infilfs_native_operation_rollback(
    struct infilfs_native_pending *pending)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(pending->sb);
    unsigned int i;
    int first_error = 0;

    for (i = 0; i < pending->undo_count; ++i) {
        int ret = infilfs_native_stage_block(pending->sb,
                                             pending->undo[i].block,
                                             pending->undo[i].data);
        if (ret && !first_error)
            first_error = ret;
    }
    memcpy(pending->tx.bitmap, pending->operation_bitmap,
           pending->operation_bitmap_bytes);
    pending->tx.next_sb = pending->operation_next_sb;
    pending->tx.free_blocks = pending->operation_free_blocks;
    pending->tx.deferred_count = pending->operation_deferred_count;
    if (pending->operation_had_last && pending->tx.deferred_count)
        pending->tx.deferred[pending->tx.deferred_count - 1u] =
            pending->operation_last_deferred;
    sbi->disk = pending->operation_next_sb;
    pending->undo_count = 0;
    if (first_error) {
        pending->commit_failed = true;
        sbi->write_poisoned = true;
    }
    return first_error;
}

static void infilfs_native_operation_commit(
    struct infilfs_native_pending *pending)
{
    pending->undo_count = 0;
}

static int infilfs_native_pending_commit_locked(
    struct infilfs_native_pending *pending)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(pending->sb);
    int ret;

    if (!pending->active)
        return pending->commit_failed ? -EIO : 0;

    pending->tx.next_sb.generation = cpu_to_le64(pending->tx.generation);
    sbi->disk = pending->tx.next_sb;
    ret = infilfs_rw_tx_commit(&pending->tx);
    if (ret) {
        pending->commit_failed = true;
        sbi->write_poisoned = true;
        pr_err("InfiltratorFS: native deferred transaction publication failed: %d\n",
               ret);
        return ret;
    }
    memset(&pending->tx, 0, sizeof(pending->tx));
    pending->active = false;
    pending->pending_bytes = 0;
    return 0;
}

static int infilfs_native_pending_flush_sb(struct super_block *sb)
{
    struct infilfs_native_pending *pending =
        infilfs_native_pending_get(sb, false);
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    int ret;

    if (!pending)
        return sync_blockdev(sb->s_bdev);
    cancel_delayed_work_sync(&pending->idle_work);
    mutex_lock(&sbi->write_lock);
    ret = infilfs_native_pending_commit_locked(pending);
    mutex_unlock(&sbi->write_lock);
    if (!ret)
        ret = sync_blockdev(sb->s_bdev);
    return ret;
}

static void infilfs_native_idle_work(struct work_struct *work)
{
    struct infilfs_native_pending *pending = container_of(
        to_delayed_work(work), struct infilfs_native_pending, idle_work);
    struct infilfs_sb_info *sbi = INFILFS_SB(pending->sb);
    int ret;

    if (!sbi)
        return;
    mutex_lock(&sbi->write_lock);
    ret = infilfs_native_pending_commit_locked(pending);
    mutex_unlock(&sbi->write_lock);
    if (ret)
        pr_err("InfiltratorFS: native idle publication failed: %d\n", ret);
}

static void infilfs_native_pending_destroy(struct super_block *sb)
{
    struct infilfs_native_pending *pending;
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);

    mutex_lock(&infilfs_native_pending_list_lock);
    pending = infilfs_native_pending_find_locked(sb);
    if (pending)
        list_del_init(&pending->node);
    mutex_unlock(&infilfs_native_pending_list_lock);

    /*
     * Read-only mounts never allocate a pending writer, but verified reads can
     * still populate the checksum cursor cache.  Invalidate before the early
     * return so no entry can retain a dead super_block pointer across unmount.
     */
    infilfs_native_checksum_cache_invalidate_sb(sb);
    if (!pending)
        return;

    cancel_delayed_work_sync(&pending->idle_work);
    if (sbi) {
        mutex_lock(&sbi->write_lock);
        if (pending->active &&
            infilfs_native_pending_commit_locked(pending) != 0)
            pr_err("InfiltratorFS: unmount could not publish pending native writes\n");
        mutex_unlock(&sbi->write_lock);
    }
    if (pending->active)
        infilfs_rw_tx_abort(&pending->tx);
    kvfree(pending->operation_bitmap);
    kfree(pending);
}

static int infilfs_native_alloc_data_exact(struct infilfs_native_pending *pending,
                                           u64 count, u64 preferred,
                                           u64 *start_out)
{
    struct infilfs_rw_tx *tx = &pending->tx;
    u64 total = tx->sbi->device_blocks;
    u64 start;
    u64 i;
    u64 run = 0;

    if (!count || count > tx->free_blocks)
        return -ENOSPC;
    if (preferred > 0 && preferred < total && count <= total - preferred) {
        for (i = 0; i < count; ++i)
            if (infilfs_rw_bitmap_get(tx->bitmap, preferred + i))
                break;
        if (i == count) {
            for (i = 0; i < count; ++i)
                infilfs_rw_bitmap_set(tx->bitmap, preferred + i, true);
            tx->free_blocks -= count;
            *start_out = preferred;
            return 0;
        }
    }

    start = 1;
    for (i = 1; i < total; ++i) {
        if (!infilfs_rw_bitmap_get(tx->bitmap, i)) {
            if (!run)
                start = i;
            run++;
            if (run == count)
                break;
        } else {
            run = 0;
        }
    }
    if (run != count)
        return -ENOSPC;
    for (i = 0; i < count; ++i)
        infilfs_rw_bitmap_set(tx->bitmap, start + i, true);
    tx->free_blocks -= count;
    *start_out = start;
    return 0;
}

static void infilfs_native_block_digest(
    const u8 data[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_data_checksum_disk *digest)
{
    struct infilfs_rw_sha256_ctx ctx;

    infilfs_rw_sha256_init(&ctx);
    infilfs_rw_sha256_update(&ctx, data, INFILFS_DISK_BLOCK_SIZE);
    infilfs_rw_sha256_final(&ctx, digest->bytes);
}

static int infilfs_native_collect_extents(
    struct infilfs_native_pending *pending, struct inode *inode,
    u8 object[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_extent_disk **extents_out, u32 *count_out,
    u64 *old_blocks_out, bool *was_inline_out)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_object_header_disk *header;
    struct infilfs_file_payload_disk *file;
    struct infilfs_extent_disk *extents = NULL;
    u32 count;
    u64 logical = 0;
    int ret;
    u32 i;

    ret = infilfs_read_object(inode->i_sb, ii->object_block,
                              INFILFS_OBJECT_FILE, ii->object_id, object);
    if (ret)
        return ret;
    header = (struct infilfs_object_header_disk *)object;
    file = (struct infilfs_file_payload_disk *)(header + 1);
    count = le32_to_cpu(file->extent_count);
    *was_inline_out = false;

    if (!count) {
        u64 size = le64_to_cpu(file->attributes.logical_size);
        if (size > INFILFS_INLINE_DATA_MAX)
            return -EFSCORRUPTED;
        *was_inline_out = size != 0;
        *extents_out = NULL;
        *count_out = 0;
        *old_blocks_out = 0;
        return 0;
    }

    extents = kvmalloc_array(count, sizeof(*extents), GFP_NOFS);
    if (!extents)
        return -ENOMEM;

    if (le16_to_cpu(header->object_version) == INFILFS_OBJECT_VERSION_CLASSIC) {
        size_t need = sizeof(*file) + (size_t)count * sizeof(*extents);
        if (need != le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto fail;
        }
        memcpy(extents, file + 1, (size_t)count * sizeof(*extents));
    } else if (le16_to_cpu(header->object_version) ==
               INFILFS_OBJECT_VERSION_PAGED) {
        struct infilfs_extent_head_disk *head =
            (struct infilfs_extent_head_disk *)(file + 1);
        __le64 *pages = (__le64 *)(head + 1);
        u32 page_count = le32_to_cpu(head->page_count);
        u32 copied = 0;
        u8 *page_block;
        u32 p;

        if (!page_count || page_count > INFILFS_EXTENT_PAGE_POINTERS ||
            le32_to_cpu(head->reserved) != 0) {
            ret = -EFSCORRUPTED;
            goto fail;
        }
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!page_block) {
            ret = -ENOMEM;
            goto fail;
        }
        for (p = 0; p < page_count; ++p) {
            struct infilfs_metadata_page_disk *page;
            u32 n;
            u64 block = le64_to_cpu(pages[p]);

            ret = infilfs_read_block(inode->i_sb, block, page_block);
            if (ret)
                break;
            if (!infilfs_metadata_page_valid(inode->i_sb, page_block,
                                             infilfs_extent_page_magic,
                                             ii->object_id)) {
                ret = -EFSCORRUPTED;
                break;
            }
            page = (struct infilfs_metadata_page_disk *)page_block;
            n = le32_to_cpu(page->entry_count);
            if (!n || n > INFILFS_EXTENTS_PER_PAGE ||
                copied > count || n > count - copied ||
                le32_to_cpu(page->bytes_used) != n * sizeof(*extents)) {
                ret = -EFSCORRUPTED;
                break;
            }
            memcpy(extents + copied, page + 1, (size_t)n * sizeof(*extents));
            copied += n;
            ret = infilfs_rw_tx_defer_free(&pending->tx, block, 1);
            if (ret)
                break;
        }
        kfree(page_block);
        if (ret)
            goto fail;
        if (copied != count) {
            ret = -EFSCORRUPTED;
            goto fail;
        }
    } else {
        ret = -EFSCORRUPTED;
        goto fail;
    }

    for (i = 0; i < count; ++i) {
        u64 start = le64_to_cpu(extents[i].logical_block);
        u64 physical = le64_to_cpu(extents[i].physical_block);
        u32 blocks = le32_to_cpu(extents[i].block_count);
        u32 flags = le32_to_cpu(extents[i].flags);

        if (!blocks || start != logical ||
            (flags != INFILFS_EXTENT_NORMAL && flags != INFILFS_EXTENT_HOLE) ||
            (flags == INFILFS_EXTENT_HOLE && physical != 0) ||
            (flags == INFILFS_EXTENT_NORMAL && physical == 0)) {
            ret = -EFSCORRUPTED;
            goto fail;
        }
        logical += blocks;
    }
    *extents_out = extents;
    *count_out = count;
    *old_blocks_out = logical;
    return 0;
fail:
    kvfree(extents);
    return ret;
}

static int infilfs_native_map_extent(const struct infilfs_extent_disk *extents,
                                     u32 count, u64 logical,
                                     u64 *physical_out, u32 *flags_out)
{
    u32 i;

    for (i = 0; i < count; ++i) {
        u64 start = le64_to_cpu(extents[i].logical_block);
        u32 blocks = le32_to_cpu(extents[i].block_count);
        u32 flags = le32_to_cpu(extents[i].flags);

        if (logical < start || logical >= start + blocks)
            continue;
        *flags_out = flags;
        *physical_out = flags == INFILFS_EXTENT_HOLE ? 0 :
            le64_to_cpu(extents[i].physical_block) + logical - start;
        return 0;
    }
    return -ENOENT;
}

static int infilfs_native_extent_append(struct infilfs_extent_disk *out,
                                        u32 capacity, u32 *count,
                                        u64 logical, u64 physical,
                                        u32 blocks, u32 flags)
{
    struct infilfs_extent_disk *last;

    if (!blocks)
        return 0;
    if (*count) {
        last = &out[*count - 1u];
        if (le64_to_cpu(last->logical_block) +
                le32_to_cpu(last->block_count) == logical &&
            le32_to_cpu(last->flags) == flags &&
            ((flags == INFILFS_EXTENT_HOLE && physical == 0) ||
             (flags == INFILFS_EXTENT_NORMAL &&
              le64_to_cpu(last->physical_block) +
                  le32_to_cpu(last->block_count) == physical)) &&
            (u64)le32_to_cpu(last->block_count) + blocks <= U32_MAX) {
            last->block_count = cpu_to_le32(
                le32_to_cpu(last->block_count) + blocks);
            return 0;
        }
    }
    if (*count >= capacity)
        return -ENOSPC;
    out[*count].logical_block = cpu_to_le64(logical);
    out[*count].physical_block = cpu_to_le64(
        flags == INFILFS_EXTENT_HOLE ? 0 : physical);
    out[*count].block_count = cpu_to_le32(blocks);
    out[*count].flags = cpu_to_le32(flags);
    (*count)++;
    return 0;
}

static int infilfs_native_replace_extent_range(
    const struct infilfs_extent_disk *old, u32 old_count, u64 old_blocks,
    u64 replace_start, u64 replace_count, u64 replacement_physical,
    u32 replacement_flags,
    struct infilfs_extent_disk **new_out, u32 *new_count_out)
{
    struct infilfs_extent_disk *out;
    u32 capacity = old_count + 4u;
    u32 count = 0;
    u64 replace_end = replace_start + replace_count;
    u32 i;
    int ret;

    out = kvmalloc_array(capacity, sizeof(*out), GFP_NOFS);
    if (!out)
        return -ENOMEM;

    for (i = 0; i < old_count; ++i) {
        u64 start = le64_to_cpu(old[i].logical_block);
        u64 end = start + le32_to_cpu(old[i].block_count);
        u64 physical = le64_to_cpu(old[i].physical_block);
        u32 flags = le32_to_cpu(old[i].flags);
        u64 keep_end;

        if (start >= replace_start)
            break;
        keep_end = min_t(u64, end, replace_start);
        ret = infilfs_native_extent_append(out, capacity, &count, start,
            flags == INFILFS_EXTENT_NORMAL ? physical : 0,
            (u32)(keep_end - start), flags);
        if (ret)
            goto fail;
        if (end >= replace_start)
            break;
    }

    if (old_blocks < replace_start) {
        ret = infilfs_native_extent_append(out, capacity, &count, old_blocks, 0,
            (u32)(replace_start - old_blocks), INFILFS_EXTENT_HOLE);
        if (ret)
            goto fail;
    }
    ret = infilfs_native_extent_append(out, capacity, &count, replace_start,
        replacement_physical, (u32)replace_count, replacement_flags);
    if (ret)
        goto fail;

    for (i = 0; i < old_count; ++i) {
        u64 start = le64_to_cpu(old[i].logical_block);
        u64 end = start + le32_to_cpu(old[i].block_count);
        u64 physical = le64_to_cpu(old[i].physical_block);
        u32 flags = le32_to_cpu(old[i].flags);
        u64 keep_start;

        if (end <= replace_end)
            continue;
        keep_start = max_t(u64, start, replace_end);
        ret = infilfs_native_extent_append(out, capacity, &count, keep_start,
            flags == INFILFS_EXTENT_NORMAL ?
                physical + (keep_start - start) : 0,
            (u32)(end - keep_start), flags);
        if (ret)
            goto fail;
    }

    *new_out = out;
    *new_count_out = count;
    return 0;
fail:
    kvfree(out);
    return ret;
}

static int infilfs_native_release_replaced_extents(
    struct infilfs_native_pending *pending, const u8 owner_id[16],
    const struct infilfs_extent_disk *extents, u32 extent_count,
    u64 replace_start, u64 replace_count)
{
    u64 replace_end;
    u32 i;

    if (!replace_count)
        return 0;
    if (replace_start > U64_MAX - replace_count)
        return -EOVERFLOW;
    replace_end = replace_start + replace_count;
    for (i = 0; i < extent_count; ++i) {
        u64 logical = le64_to_cpu(extents[i].logical_block);
        u64 extent_end = logical + le32_to_cpu(extents[i].block_count);
        u64 first;
        u64 last;
        u64 physical;
        int ret;

        if (le32_to_cpu(extents[i].flags) != INFILFS_EXTENT_NORMAL ||
            extent_end <= replace_start || logical >= replace_end)
            continue;
        first = max_t(u64, logical, replace_start);
        last = min_t(u64, extent_end, replace_end);
        physical = le64_to_cpu(extents[i].physical_block) + first - logical;
        ret = infilfs_ns_free_unshared_run(pending, owner_id, physical,
                                           last - first);
        if (ret)
            return ret;
    }
    return 0;
}

static int infilfs_native_random_id(u8 id[16])
{
    unsigned int attempt;

    for (attempt = 0; attempt < 8; ++attempt) {
        u8 combined = 0;
        unsigned int i;
        get_random_bytes(id, 16);
        for (i = 0; i < 16; ++i)
            combined |= id[i];
        if (combined)
            return 0;
    }
    return -EAGAIN;
}

static int infilfs_native_checksum_decode(
    struct super_block *sb, const u8 owner_id[16], const u8 object_id[16],
    u64 object_block, u8 block[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_native_checksum_payload_disk **payload_out)
{
    struct infilfs_object_header_disk *header;
    struct infilfs_native_checksum_payload_disk *payload;
    u32 count;
    size_t need;
    int ret;

    ret = infilfs_read_object(sb, object_block, INFILFS_OBJECT_CHECKSUM,
                              object_id, block);
    if (ret)
        return ret;
    header = (struct infilfs_object_header_disk *)block;
    if (le16_to_cpu(header->object_version) != INFILFS_OBJECT_VERSION_CLASSIC ||
        le32_to_cpu(header->payload_size) < sizeof(*payload))
        return -EFSCORRUPTED;
    payload = (struct infilfs_native_checksum_payload_disk *)(header + 1);
    count = le32_to_cpu(payload->checksum_count);
    need = sizeof(*payload) + (size_t)count *
        sizeof(struct infilfs_data_checksum_disk);
    if (memcmp(payload->owner_object_id, owner_id, 16) != 0 ||
        le32_to_cpu(payload->reserved) != 0 ||
        count > INFILFS_NATIVE_CHECKSUMS_PER_OBJECT ||
        need != le32_to_cpu(header->payload_size))
        return -EFSCORRUPTED;
    *payload_out = payload;
    return 0;
}

static int infilfs_native_checksum_tail(
    struct super_block *sb, const u8 owner_id[16], const u8 head_id[16],
    u8 tail_id[16], u64 *tail_block_out, u64 *tail_start_out,
    u8 tail_object[INFILFS_DISK_BLOCK_SIZE])
{
    struct infilfs_native_checksum_cache_entry cached;
    struct infilfs_native_checksum_payload_disk *payload;
    u8 current_id[16];
    u64 block = 0;
    u64 previous_start = 0;
    bool have_previous = false;
    unsigned int guard;
    int ret;

    if (!memcmp(head_id, (u8[16]){0}, 16))
        return -ENOENT;

    if (infilfs_native_checksum_cache_lookup(sb, owner_id, &cached)) {
        u64 indexed_block = 0;
        u16 indexed_type = 0;

        memcpy(current_id, cached.object_id, 16);
        block = cached.object_block;
        /*
         * Readers and appenders deliberately share this bounded cache.  A
         * reader that started before a checksum-tail CoW may publish the old
         * physical block after the writer has cached the replacement.  The
         * old block remains structurally valid, so decoding it alone is not a
         * sufficient freshness check.  Require the live object index to map
         * the cached object ID to the same block before the append path trusts
         * it; otherwise restart from the authoritative checksum head.
         */
        ret = infilfs_index_lookup(sb, current_id, &indexed_block,
                                   &indexed_type);
        if (!ret && (indexed_type != INFILFS_OBJECT_CHECKSUM ||
                     indexed_block != block))
            ret = -ESTALE;
        if (!ret)
            ret = infilfs_native_checksum_decode(sb, owner_id, current_id,
                                                  block, tail_object,
                                                  &payload);
        if (ret) {
            memcpy(current_id, head_id, 16);
            block = 0;
        } else {
            previous_start = le64_to_cpu(payload->start_logical_block);
            have_previous = true;
        }
    } else {
        memcpy(current_id, head_id, 16);
    }

    for (guard = 0; guard < 1048576u; ++guard) {
        u64 start;
        u8 next_id[16];
        u16 type;

        if (!block) {
            ret = infilfs_index_lookup(sb, current_id, &block, &type);
            if (ret)
                return ret;
            if (type != INFILFS_OBJECT_CHECKSUM)
                return -EFSCORRUPTED;
            ret = infilfs_native_checksum_decode(sb, owner_id, current_id,
                                                  block, tail_object, &payload);
            if (ret)
                return ret;
        }
        start = le64_to_cpu(payload->start_logical_block);
        if ((start % INFILFS_NATIVE_CHECKSUMS_PER_OBJECT) != 0 ||
            (have_previous && start < previous_start))
            return -EFSCORRUPTED;
        memcpy(next_id, payload->next_object_id, 16);
        if (!memcmp(next_id, (u8[16]){0}, 16)) {
            memcpy(tail_id, current_id, 16);
            *tail_block_out = block;
            *tail_start_out = start;
            return 0;
        }
        previous_start = start;
        have_previous = true;
        memcpy(current_id, next_id, 16);
        block = 0;
    }
    return -ELOOP;
}

static int infilfs_native_init_checksum_object(
    struct infilfs_native_pending *pending, const u8 owner_id[16],
    const u8 object_id[16], const u8 next_id[16], u64 group_start,
    const struct infilfs_data_checksum_disk *digests, u64 touched_start,
    u64 touched_count, u8 block[INFILFS_DISK_BLOCK_SIZE])
{
    struct infilfs_object_header_disk *header;
    struct infilfs_native_checksum_payload_disk *payload;
    struct infilfs_data_checksum_disk *values;
    u64 first;
    u64 last;
    u32 count;

    memset(block, 0, INFILFS_DISK_BLOCK_SIZE);
    header = (struct infilfs_object_header_disk *)block;
    memcpy(header->magic, infilfs_object_magic, 8);
    header->object_type = cpu_to_le16(INFILFS_OBJECT_CHECKSUM);
    header->object_version = cpu_to_le16(INFILFS_OBJECT_VERSION_CLASSIC);
    header->header_size = cpu_to_le32(sizeof(*header));
    header->generation = cpu_to_le64(pending->tx.generation);
    memcpy(header->object_id, object_id, 16);
    memcpy(header->parent_id, owner_id, 16);
    header->checksum_type = cpu_to_le32(INFILFS_CHECKSUM_CRC64_ECMA);
    payload = (struct infilfs_native_checksum_payload_disk *)(header + 1);
    memcpy(payload->owner_object_id, owner_id, 16);
    if (next_id)
        memcpy(payload->next_object_id, next_id, 16);
    payload->start_logical_block = cpu_to_le64(group_start);
    values = (struct infilfs_data_checksum_disk *)(payload + 1);

    first = max_t(u64, group_start, touched_start);
    last = min_t(u64, group_start + INFILFS_NATIVE_CHECKSUMS_PER_OBJECT,
                 touched_start + touched_count);
    if (last <= first)
        return -EINVAL;
    memcpy(values + (first - group_start), digests + (first - touched_start),
           (size_t)(last - first) * sizeof(*values));
    count = (u32)(last - group_start);
    payload->checksum_count = cpu_to_le32(count);
    header->payload_size = cpu_to_le32(sizeof(*payload) +
        (size_t)count * sizeof(*values));
    return infilfs_rw_finalize_object(block);
}

static int infilfs_native_checksum_collect(
    struct infilfs_native_pending *pending, const u8 owner_id[16],
    const u8 head_id[16], struct infilfs_native_checksum_node **nodes_out,
    u32 *count_out)
{
    struct infilfs_native_checksum_node *nodes = NULL;
    u32 count = 0;
    u32 capacity = 0;
    u8 current_id[16];
    u64 previous_start = 0;
    bool have_previous = false;
    unsigned int guard;
    int ret = 0;

    *nodes_out = NULL;
    *count_out = 0;
    memcpy(current_id, head_id, sizeof(current_id));
    for (guard = 0; memchr_inv(current_id, 0, sizeof(current_id)) &&
         guard < 1048576u; ++guard) {
        struct infilfs_native_checksum_payload_disk *payload;
        struct infilfs_native_checksum_node *grown;
        u8 *object;
        u8 next_id[16];
        u64 block;
        u64 start;
        u32 checksum_count;
        u16 type;

        ret = infilfs_index_lookup(pending->sb, current_id, &block, &type);
        if (ret)
            goto out;
        if (type != INFILFS_OBJECT_CHECKSUM) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!object) {
            ret = -ENOMEM;
            goto out;
        }
        ret = infilfs_native_checksum_decode(pending->sb, owner_id,
                                             current_id, block, object,
                                             &payload);
        if (!ret) {
            start = le64_to_cpu(payload->start_logical_block);
            checksum_count = le32_to_cpu(payload->checksum_count);
            memcpy(next_id, payload->next_object_id, sizeof(next_id));
            if ((start % INFILFS_NATIVE_CHECKSUMS_PER_OBJECT) != 0 ||
                (have_previous && start <= previous_start) ||
                !checksum_count)
                ret = -EFSCORRUPTED;
        }
        kfree(object);
        if (ret)
            goto out;

        if (count == capacity) {
            u32 new_capacity = capacity ? capacity * 2u : 16u;

            grown = kvmalloc_array(new_capacity, sizeof(*grown), GFP_NOFS);
            if (!grown) {
                ret = -ENOMEM;
                goto out;
            }
            if (count)
                memcpy(grown, nodes, (size_t)count * sizeof(*grown));
            kvfree(nodes);
            nodes = grown;
            capacity = new_capacity;
        }
        memset(&nodes[count], 0, sizeof(nodes[count]));
        memcpy(nodes[count].object_id, current_id, 16);
        nodes[count].object_block = block;
        nodes[count].start_logical = start;
        nodes[count].checksum_count = checksum_count;
        count++;
        previous_start = start;
        have_previous = true;
        memcpy(current_id, next_id, sizeof(current_id));
    }
    if (memchr_inv(current_id, 0, sizeof(current_id))) {
        ret = -ELOOP;
        goto out;
    }
    *nodes_out = nodes;
    *count_out = count;
    return 0;
out:
    kvfree(nodes);
    return ret;
}

static int infilfs_native_checksum_insert_node(
    struct infilfs_native_checksum_node **nodes_inout, u32 *count_inout,
    u32 index, u64 start)
{
    struct infilfs_native_checksum_node *old = *nodes_inout;
    struct infilfs_native_checksum_node *grown;
    u32 count = *count_inout;
    int ret;

    grown = kvmalloc_array((size_t)count + 1u, sizeof(*grown), GFP_NOFS);
    if (!grown)
        return -ENOMEM;
    if (index)
        memcpy(grown, old, (size_t)index * sizeof(*grown));
    if (index < count)
        memcpy(grown + index + 1u, old + index,
               (size_t)(count - index) * sizeof(*grown));
    memset(&grown[index], 0, sizeof(grown[index]));
    ret = infilfs_native_random_id(grown[index].object_id);
    if (ret) {
        kvfree(grown);
        return ret;
    }
    grown[index].start_logical = start;
    grown[index].new_node = true;
    grown[index].dirty = true;
    if (index)
        grown[index - 1u].dirty = true;
    kvfree(old);
    *nodes_inout = grown;
    *count_inout = count + 1u;
    return 0;
}

/*
 * Apply checksum changes to any logical range, not merely to the current tail.
 * The chain is ordered by checksum group.  Existing affected objects are
 * copy-on-written, missing sparse groups are inserted in order, and the one
 * predecessor whose next pointer changes is rewritten in the same operation.
 */
static int infilfs_native_checksum_set_range(
    struct infilfs_native_pending *pending,
    struct infilfs_file_payload_disk *file, const u8 owner_id[16],
    u64 touched_start, u64 touched_count,
    const struct infilfs_data_checksum_disk *digests,
    struct infilfs_native_index_change *changes, u32 change_capacity,
    u32 *change_count, u8 final_tail_id[16], u64 *final_tail_block,
    u64 *final_tail_start)
{
    const u64 per = INFILFS_NATIVE_CHECKSUMS_PER_OBJECT;
    struct infilfs_native_checksum_node *nodes = NULL;
    u8 *object = NULL;
    u64 first_group;
    u64 last_group;
    u64 group;
    u32 node_count = 0;
    u32 i;
    int ret = 0;

    if (!touched_count || touched_start > U64_MAX - touched_count)
        return -EINVAL;
    first_group = (touched_start / per) * per;
    last_group = ((touched_start + touched_count - 1u) / per) * per;

    ret = infilfs_native_checksum_collect(pending, owner_id,
                                          file->checksum_head_id,
                                          &nodes, &node_count);
    if (ret)
        return ret;

    for (group = first_group; ; group += per) {
        u32 at = 0;

        while (at < node_count && nodes[at].start_logical < group)
            ++at;
        if (at == node_count || nodes[at].start_logical != group) {
            ret = infilfs_native_checksum_insert_node(&nodes, &node_count,
                                                      at, group);
            if (ret)
                goto out;
        }
        nodes[at].dirty = true;
        if (group == last_group)
            break;
        if (group > U64_MAX - per) {
            ret = -EOVERFLOW;
            goto out;
        }
    }

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object) {
        ret = -ENOMEM;
        goto out;
    }

    for (i = 0; i < node_count; ++i) {
        struct infilfs_native_checksum_payload_disk *payload;
        struct infilfs_data_checksum_disk *values;
        const u8 *next_id = i + 1u < node_count ?
            nodes[i + 1u].object_id : NULL;
        u64 group_start = nodes[i].start_logical;
        u64 first = max_t(u64, group_start, touched_start);
        u64 last = min_t(u64, group_start + per,
                         touched_start + touched_count);
        u64 stored_block;
        u32 needed = nodes[i].checksum_count;

        if (!nodes[i].dirty)
            continue;
        if (nodes[i].new_node) {
            needed = (u32)(last - group_start);
            memset(object, 0, INFILFS_DISK_BLOCK_SIZE);
            ret = infilfs_native_init_checksum_object(
                pending, owner_id, nodes[i].object_id, next_id, group_start,
                digests, touched_start, touched_count, object);
            if (ret)
                goto out;
            ret = infilfs_rw_tx_alloc(&pending->tx, 1, &stored_block);
            if (ret)
                goto out;
            ret = infilfs_native_stage_block(pending->sb, stored_block,
                                             object);
            if (ret)
                goto out;
        } else {
            ret = infilfs_native_checksum_decode(
                pending->sb, owner_id, nodes[i].object_id,
                nodes[i].object_block, object, &payload);
            if (ret)
                goto out;
            values = (struct infilfs_data_checksum_disk *)(payload + 1);
            if (last > first) {
                memcpy(values + (first - group_start),
                       digests + (first - touched_start),
                       (size_t)(last - first) * sizeof(*values));
                needed = max_t(u32, needed,
                               (u32)(last - group_start));
            }
            memset(payload->next_object_id, 0,
                   sizeof(payload->next_object_id));
            if (next_id)
                memcpy(payload->next_object_id, next_id, 16);
            payload->checksum_count = cpu_to_le32(needed);
            ((struct infilfs_object_header_disk *)object)->payload_size =
                cpu_to_le32(sizeof(*payload) +
                    (size_t)needed * sizeof(*values));
            ((struct infilfs_object_header_disk *)object)->generation =
                cpu_to_le64(pending->tx.generation);
            ret = infilfs_rw_finalize_object(object);
            if (ret)
                goto out;
            ret = infilfs_native_store_private_or_cow(
                pending, nodes[i].object_block, object, &stored_block);
            if (ret)
                goto out;
        }
        if (*change_count >= change_capacity) {
            ret = -ENOSPC;
            goto out;
        }
        memcpy(changes[*change_count].object_id, nodes[i].object_id, 16);
        changes[*change_count].object_block = stored_block;
        changes[*change_count].object_type = INFILFS_OBJECT_CHECKSUM;
        changes[*change_count].add = nodes[i].new_node;
        (*change_count)++;
        nodes[i].object_block = stored_block;
        nodes[i].checksum_count = needed;
    }

    if (!node_count) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    memcpy(file->checksum_head_id, nodes[0].object_id, 16);
    memcpy(final_tail_id, nodes[node_count - 1u].object_id, 16);
    *final_tail_block = nodes[node_count - 1u].object_block;
    *final_tail_start = nodes[node_count - 1u].start_logical;
out:
    kfree(object);
    kvfree(nodes);
    return ret;
}

static __maybe_unused int infilfs_native_checksum_append(
    struct infilfs_native_pending *pending,
    struct infilfs_file_payload_disk *file, const u8 owner_id[16],
    u64 touched_start, u64 touched_count,
    const struct infilfs_data_checksum_disk *digests,
    struct infilfs_native_index_change *changes, u32 change_capacity,
    u32 *change_count, u8 final_tail_id[16], u64 *final_tail_block,
    u64 *final_tail_start)
{
    return infilfs_native_checksum_set_range(
        pending, file, owner_id, touched_start, touched_count, digests,
        changes, change_capacity, change_count, final_tail_id,
        final_tail_block, final_tail_start);
}

static int infilfs_native_index_update_classic(
    struct infilfs_native_pending *pending,
    struct infilfs_native_index_change *changes, u32 change_count)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(pending->sb);
    u32 i;
    int ret = 0;

    for (i = 0; i < change_count; ++i) {
        if (changes[i].add)
            ret = infilfs_rw_rebuild_index(&pending->tx, NULL, 0, 0,
                changes[i].object_id, changes[i].object_block,
                changes[i].object_type);
        else
            ret = infilfs_rw_rebuild_index(&pending->tx,
                changes[i].object_id, changes[i].object_block,
                changes[i].object_type, NULL, 0, 0);
        if (ret)
            return ret;
        pending->tx.next_sb.generation = cpu_to_le64(pending->tx.generation);
        sbi->disk = pending->tx.next_sb;
    }
    return 0;
}

static int infilfs_native_index_update_paged(
    struct infilfs_native_pending *pending,
    struct infilfs_native_index_change *changes, u32 change_count,
    u8 head[INFILFS_DISK_BLOCK_SIZE])
{
    struct infilfs_sb_info *sbi = INFILFS_SB(pending->sb);
    struct infilfs_object_header_disk *header =
        (struct infilfs_object_header_disk *)head;
    struct infilfs_index_payload_disk *payload =
        (struct infilfs_index_payload_disk *)(header + 1);
    __le64 *pages = (__le64 *)(payload + 1);
    u32 page_count = le32_to_cpu(payload->reserved);
    u32 total_count = le32_to_cpu(payload->entry_count);
    u32 p;
    u32 c;
    u8 *page_block;
    u64 old_head_block = le64_to_cpu(sbi->disk.object_index_block);
    u64 new_head_block;
    int ret = 0;

    if (page_count > INFILFS_INDEX_PAGE_POINTERS)
        return -EFSCORRUPTED;
    page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!page_block)
        return -ENOMEM;

    for (p = 0; p < page_count; ++p) {
        struct infilfs_metadata_page_disk *page;
        struct infilfs_index_entry_disk *entries;
        u32 n;
        bool dirty = false;
        u64 page_no = le64_to_cpu(pages[p]);
        u64 replacement;
        u32 i;

        ret = infilfs_read_block(pending->sb, page_no, page_block);
        if (ret)
            goto out;
        if (!infilfs_metadata_page_valid(pending->sb, page_block,
                                         infilfs_index_page_magic,
                                         header->object_id)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        page = (struct infilfs_metadata_page_disk *)page_block;
        n = le32_to_cpu(page->entry_count);
        if (n > INFILFS_INDEX_ENTRIES_PER_PAGE ||
            le32_to_cpu(page->bytes_used) != n * sizeof(*entries)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        entries = (struct infilfs_index_entry_disk *)(page + 1);
        for (i = 0; i < n; ++i) {
            for (c = 0; c < change_count; ++c) {
                if (memcmp(entries[i].object_id, changes[c].object_id, 16) != 0)
                    continue;
                if (changes[c].add) {
                    ret = -EEXIST;
                    goto out;
                }
                entries[i].object_block = cpu_to_le64(changes[c].object_block);
                entries[i].object_type = cpu_to_le16(changes[c].object_type);
                changes[c].found = true;
                dirty = true;
            }
        }
        if (!dirty)
            continue;
        page->generation = cpu_to_le64(pending->tx.generation);
        ret = infilfs_rw_finalize_page(page_block);
        if (ret)
            goto out;
        ret = infilfs_native_store_private_or_cow(
            pending, page_no, page_block, &replacement);
        if (ret)
            goto out;
        pages[p] = cpu_to_le64(replacement);
    }

    for (c = 0; c < change_count; ++c)
        if (!changes[c].add && !changes[c].found) {
            ret = -ENOENT;
            goto out;
        }

    c = 0;
    while (c < change_count) {
        struct infilfs_metadata_page_disk *page;
        struct infilfs_index_entry_disk *entries;
        u32 n = 0;
        u64 page_no = 0;
        u64 replacement;

        while (c < change_count && !changes[c].add)
            ++c;
        if (c >= change_count)
            break;

        if (page_count) {
            page_no = le64_to_cpu(pages[page_count - 1u]);
            ret = infilfs_read_block(pending->sb, page_no, page_block);
            if (ret)
                goto out;
            if (!infilfs_metadata_page_valid(pending->sb, page_block,
                                             infilfs_index_page_magic,
                                             header->object_id)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            page = (struct infilfs_metadata_page_disk *)page_block;
            n = le32_to_cpu(page->entry_count);
            if (n > INFILFS_INDEX_ENTRIES_PER_PAGE) {
                ret = -EFSCORRUPTED;
                goto out;
            }
        } else {
            infilfs_rw_init_page(page_block, infilfs_index_page_magic,
                                 header->object_id, pending->tx.generation);
            page = (struct infilfs_metadata_page_disk *)page_block;
        }

        if (n == INFILFS_INDEX_ENTRIES_PER_PAGE) {
            if (page_count >= INFILFS_INDEX_PAGE_POINTERS) {
                ret = -ENOSPC;
                goto out;
            }
            infilfs_rw_init_page(page_block, infilfs_index_page_magic,
                                 header->object_id, pending->tx.generation);
            page = (struct infilfs_metadata_page_disk *)page_block;
            n = 0;
            page_no = 0;
        }
        entries = (struct infilfs_index_entry_disk *)(page + 1);
        while (c < change_count && n < INFILFS_INDEX_ENTRIES_PER_PAGE) {
            if (!changes[c].add) {
                ++c;
                continue;
            }
            memset(&entries[n], 0, sizeof(entries[n]));
            memcpy(entries[n].object_id, changes[c].object_id, 16);
            entries[n].object_block = cpu_to_le64(changes[c].object_block);
            entries[n].object_type = cpu_to_le16(changes[c].object_type);
            changes[c].found = true;
            n++;
            total_count++;
            ++c;
        }
        page->entry_count = cpu_to_le32(n);
        page->bytes_used = cpu_to_le32(n * sizeof(*entries));
        page->generation = cpu_to_le64(pending->tx.generation);
        ret = infilfs_rw_finalize_page(page_block);
        if (ret)
            goto out;
        ret = infilfs_native_store_private_or_cow(
            pending, page_no, page_block, &replacement);
        if (ret)
            goto out;
        if (!page_no) {
            pages[page_count++] = cpu_to_le64(replacement);
            payload->reserved = cpu_to_le32(page_count);
        } else {
            pages[page_count - 1u] = cpu_to_le64(replacement);
        }
    }

    payload->entry_count = cpu_to_le32(total_count);
    header->generation = cpu_to_le64(pending->tx.generation);
    header->payload_size = cpu_to_le32(sizeof(*payload) +
        (size_t)page_count * sizeof(__le64));
    ret = infilfs_rw_finalize_object(head);
    if (ret)
        goto out;
    ret = infilfs_native_store_private_or_cow(
        pending, old_head_block, head, &new_head_block);
    if (ret)
        goto out;
    pending->tx.next_sb.object_index_block = cpu_to_le64(new_head_block);
out:
    kfree(page_block);
    return ret;
}

static int infilfs_native_index_update(
    struct infilfs_native_pending *pending,
    struct infilfs_native_index_change *changes, u32 change_count)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(pending->sb);
    u8 *head;
    struct infilfs_object_header_disk *header;
    int ret;

    if (!change_count)
        return 0;
    head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!head)
        return -ENOMEM;
    ret = infilfs_read_object(pending->sb,
                              le64_to_cpu(sbi->disk.object_index_block),
                              INFILFS_OBJECT_INDEX, NULL, head);
    if (ret)
        goto out;
    header = (struct infilfs_object_header_disk *)head;
    if (le16_to_cpu(header->object_version) == INFILFS_OBJECT_VERSION_PAGED)
        ret = infilfs_native_index_update_paged(pending, changes,
                                                change_count, head);
    else
        ret = infilfs_native_index_update_classic(pending, changes,
                                                  change_count);
out:
    kfree(head);
    return ret;
}

static int infilfs_native_build_file_object(
    struct infilfs_native_pending *pending, struct inode *inode,
    const u8 old_object[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_file_payload_disk *working_file,
    const struct infilfs_extent_disk *extents, u32 extent_count,
    u64 new_size, u8 new_object[INFILFS_DISK_BLOCK_SIZE],
    u64 *new_object_block, bool *repoint_needed)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    const struct infilfs_object_header_disk *old_header =
        (const struct infilfs_object_header_disk *)old_object;
    struct infilfs_object_header_disk *header;
    struct infilfs_file_payload_disk *file;
    const u32 classic_max = (INFILFS_DISK_BLOCK_SIZE -
        sizeof(struct infilfs_object_header_disk) -
        sizeof(struct infilfs_file_payload_disk)) /
        sizeof(struct infilfs_extent_disk);
    bool paged = extent_count && (extent_count > classic_max ||
        le16_to_cpu(old_header->object_version) == INFILFS_OBJECT_VERSION_PAGED);
    u64 stored_block;
    int ret;

    memset(new_object, 0, INFILFS_DISK_BLOCK_SIZE);
    memcpy(new_object, old_object,
           sizeof(struct infilfs_object_header_disk) +
           sizeof(struct infilfs_file_payload_disk));
    header = (struct infilfs_object_header_disk *)new_object;
    file = (struct infilfs_file_payload_disk *)(header + 1);
    memcpy(file->checksum_head_id, working_file->checksum_head_id, 16);
    file->attributes.logical_size = cpu_to_le64(new_size);
    file->attributes.modification_time_ns = cpu_to_le64(ktime_get_real_ns());
    file->attributes.change_time_ns = file->attributes.modification_time_ns;
    file->extent_count = cpu_to_le32(extent_count);
    file->data_checksum_type = cpu_to_le32(INFILFS_CHECKSUM_SHA256);
    header->generation = cpu_to_le64(pending->tx.generation);

    if (!paged) {
        header->object_version = cpu_to_le16(INFILFS_OBJECT_VERSION_CLASSIC);
        header->payload_size = cpu_to_le32(sizeof(*file) +
            (size_t)extent_count * sizeof(*extents));
        if (extent_count)
            memcpy(file + 1, extents, (size_t)extent_count * sizeof(*extents));
    } else {
        struct infilfs_extent_head_disk *head =
            (struct infilfs_extent_head_disk *)(file + 1);
        __le64 *pointers = (__le64 *)(head + 1);
        u32 pages = DIV_ROUND_UP(extent_count, INFILFS_EXTENTS_PER_PAGE);
        u32 copied = 0;
        u32 p;
        u8 *page_block;

        if (!(le64_to_cpu(pending->tx.next_sb.incompat_flags) &
              INFILFS_INCOMPAT_PAGED_EXTENTS) ||
            !pages || pages > INFILFS_EXTENT_PAGE_POINTERS)
            return -EFBIG;
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!page_block)
            return -ENOMEM;
        header->object_version = cpu_to_le16(INFILFS_OBJECT_VERSION_PAGED);
        head->page_count = cpu_to_le32(pages);
        head->reserved = 0;
        for (p = 0; p < pages; ++p) {
            struct infilfs_metadata_page_disk *page;
            u32 n = min_t(u32, INFILFS_EXTENTS_PER_PAGE,
                          extent_count - copied);
            u64 block;

            infilfs_rw_init_page(page_block, infilfs_extent_page_magic,
                                 ii->object_id, pending->tx.generation);
            page = (struct infilfs_metadata_page_disk *)page_block;
            page->entry_count = cpu_to_le32(n);
            page->bytes_used = cpu_to_le32(n * sizeof(*extents));
            memcpy(page + 1, extents + copied, (size_t)n * sizeof(*extents));
            ret = infilfs_rw_finalize_page(page_block);
            if (ret) {
                kfree(page_block);
                return ret;
            }
            ret = infilfs_rw_tx_alloc(&pending->tx, 1, &block);
            if (ret) {
                kfree(page_block);
                return ret;
            }
            ret = infilfs_native_stage_block(pending->sb, block, page_block);
            if (ret) {
                kfree(page_block);
                return ret;
            }
            pointers[p] = cpu_to_le64(block);
            copied += n;
        }
        kfree(page_block);
        header->payload_size = cpu_to_le32(sizeof(*file) + sizeof(*head) +
            (size_t)pages * sizeof(__le64));
    }

    ret = infilfs_rw_finalize_object(new_object);
    if (ret)
        return ret;
    ret = infilfs_native_store_private_or_cow(
        pending, ii->object_block, new_object, &stored_block);
    if (ret)
        return ret;
    *new_object_block = stored_block;
    *repoint_needed = stored_block != ii->object_block;
    return 0;
}

static int infilfs_native_append_chunk_locked(
    struct infilfs_native_pending *pending, struct inode *inode,
    struct iov_iter *from, u64 pos, size_t bytes, size_t *consumed_out,
    u64 *new_file_block_out, u64 *new_size_out, u64 *allocated_blocks_out,
    u8 final_tail_id[16], u64 *final_tail_block, u64 *final_tail_start)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    u8 *old_object = NULL;
    u8 *new_object = NULL;
    struct infilfs_object_header_disk *old_header;
    struct infilfs_file_payload_disk *old_file;
    struct infilfs_file_payload_disk working_file;
    struct infilfs_extent_disk *old_extents = NULL;
    struct infilfs_extent_disk *new_extents = NULL;
    struct infilfs_data_checksum_disk *digests = NULL;
    u8 *data_block = NULL;
    const u32 change_capacity = 32u;
    struct infilfs_native_index_change *changes = NULL;
    u32 change_count = 0;
    u32 old_extent_count = 0;
    u32 new_extent_count = 0;
    u64 old_blocks = 0;
    bool was_inline = false;
    u64 old_size;
    u64 request_end;
    u64 new_size;
    u64 start_block;
    u64 end_block;
    u64 touched_blocks;
    u64 preferred = 1;
    u64 data_start;
    u64 logical;
    size_t consumed = 0;
    bool file_repoint;
    int ret;

    *consumed_out = 0;
    old_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    new_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    changes = kcalloc(change_capacity, sizeof(*changes), GFP_NOFS);
    if (!old_object || !new_object || !changes) {
        ret = -ENOMEM;
        goto out;
    }
    ret = infilfs_native_collect_extents(pending, inode, old_object,
        &old_extents, &old_extent_count, &old_blocks, &was_inline);
    if (ret)
        goto out;
    old_header = (struct infilfs_object_header_disk *)old_object;
    old_file = (struct infilfs_file_payload_disk *)(old_header + 1);
    old_size = le64_to_cpu(old_file->attributes.logical_size);
    if (bytes > U64_MAX - pos) {
        ret = -EOVERFLOW;
        goto out;
    }
    request_end = pos + bytes;
    new_size = max_t(u64, old_size, request_end);
    start_block = pos >> INFILFS_DISK_BLOCK_SHIFT;
    end_block = (request_end - 1u) >> INFILFS_DISK_BLOCK_SHIFT;
    touched_blocks = end_block - start_block + 1u;
    if (touched_blocks > U32_MAX) {
        ret = -EFBIG;
        goto out;
    }

    if (old_extent_count) {
        struct infilfs_extent_disk *last = &old_extents[old_extent_count - 1u];
        if (le32_to_cpu(last->flags) == INFILFS_EXTENT_NORMAL)
            preferred = le64_to_cpu(last->physical_block) +
                le32_to_cpu(last->block_count);
    }
    ret = infilfs_native_alloc_data_exact(pending, touched_blocks,
                                          preferred, &data_start);
    if (ret)
        goto out;
    digests = kvmalloc_array(touched_blocks, sizeof(*digests), GFP_NOFS);
    if (!digests) {
        ret = -ENOMEM;
        goto out;
    }

    data_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!data_block) {
        ret = -ENOMEM;
        goto out;
    }

    for (logical = start_block; logical <= end_block; ++logical) {
        u64 block_begin = logical << INFILFS_DISK_BLOCK_SHIFT;
        u64 write_begin = max_t(u64, pos, block_begin);
        u64 write_end = min_t(u64, request_end,
                              block_begin + INFILFS_DISK_BLOCK_SIZE);
        size_t offset = (size_t)(write_begin - block_begin);
        size_t take = (size_t)(write_end - write_begin);
        u64 old_physical = 0;
        u32 old_flags = INFILFS_EXTENT_HOLE;

        memset(data_block, 0, INFILFS_DISK_BLOCK_SIZE);
        if (was_inline && logical == 0 && old_size) {
            const u8 *inline_bytes = (const u8 *)(old_file + 1) +
                sizeof(struct infilfs_data_checksum_disk);
            memcpy(data_block, inline_bytes, (size_t)old_size);
        } else if (logical < old_blocks &&
                   infilfs_native_map_extent(old_extents, old_extent_count,
                                             logical, &old_physical,
                                             &old_flags) == 0 &&
                   old_flags == INFILFS_EXTENT_NORMAL) {
            ret = infilfs_read_block(inode->i_sb, old_physical, data_block);
            if (ret)
                goto out;
        }
        if (copy_from_iter(data_block + offset, take, from) != take) {
            ret = -EFAULT;
            goto out;
        }
        consumed += take;
        ret = infilfs_native_stage_block(inode->i_sb,
                                         data_start + logical - start_block,
                                         data_block);
        if (ret)
            goto out;
        infilfs_native_block_digest(data_block,
                                    &digests[logical - start_block]);
    }

    ret = infilfs_native_replace_extent_range(
        old_extents, old_extent_count, old_blocks, start_block,
        touched_blocks, data_start, INFILFS_EXTENT_NORMAL,
        &new_extents, &new_extent_count);
    if (ret)
        goto out;
    ret = infilfs_native_release_replaced_extents(
        pending, ii->object_id, old_extents, old_extent_count, start_block,
        touched_blocks);
    if (ret)
        goto out;

    memcpy(&working_file, old_file, sizeof(working_file));
    ret = infilfs_native_checksum_append(
        pending, &working_file, ii->object_id, start_block, touched_blocks,
        digests, changes, change_capacity, &change_count,
        final_tail_id, final_tail_block, final_tail_start);
    if (ret)
        goto out;

    ret = infilfs_native_build_file_object(
        pending, inode, old_object, &working_file, new_extents,
        new_extent_count, new_size, new_object, new_file_block_out,
        &file_repoint);
    if (ret)
        goto out;
    if (file_repoint) {
        if (change_count >= change_capacity) {
            ret = -ENOSPC;
            goto out;
        }
        memcpy(changes[change_count].object_id, ii->object_id, 16);
        changes[change_count].object_block = *new_file_block_out;
        changes[change_count].object_type = INFILFS_OBJECT_FILE;
        changes[change_count].add = false;
        change_count++;
    }

    ret = infilfs_native_index_update(pending, changes, change_count);
    if (ret)
        goto out;
    pending->tx.next_sb.generation = cpu_to_le64(pending->tx.generation);
    *new_size_out = new_size;
    *allocated_blocks_out = 0;
    for (logical = 0; logical < new_extent_count; ++logical)
        if (le32_to_cpu(new_extents[logical].flags) == INFILFS_EXTENT_NORMAL)
            *allocated_blocks_out +=
                le32_to_cpu(new_extents[logical].block_count);
    ret = 0;
out:
    if (ret && consumed)
        iov_iter_revert(from, consumed);
    *consumed_out = ret ? 0 : consumed;
    kfree(data_block);
    kfree(changes);
    kvfree(digests);
    kvfree(new_extents);
    kvfree(old_extents);
    kfree(new_object);
    kfree(old_object);
    return ret;
}

static ssize_t infilfs_native_extent_write_iter(struct inode *inode,
                                                loff_t *position,
                                                struct iov_iter *from,
                                                size_t requested)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(inode->i_sb);
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_native_pending *pending;
    size_t done = 0;
    loff_t pos = *position;
    int ret = 0;

    if (pos < 0)
        return -EINVAL;
    if (!requested)
        return 0;
    pending = infilfs_native_pending_get(inode->i_sb, true);
    if (!pending)
        return -ENOMEM;

    mutex_lock(&sbi->write_lock);
    ret = infilfs_native_pending_begin_locked(pending);
    if (ret)
        goto out_unlock;

    while (done < requested) {
        size_t chunk = min_t(size_t, requested - done,
                             INFILFS_NATIVE_WRITE_CHUNK);
        size_t consumed = 0;
        u64 new_file_block = 0;
        u64 new_size = 0;
        u64 allocated_blocks = 0;
        u8 tail_id[16];
        u64 tail_block = 0;
        u64 tail_start = 0;

        ret = infilfs_native_operation_snapshot(pending);
        if (ret)
            break;
        ret = infilfs_native_append_chunk_locked(
            pending, inode, from, (u64)pos + done, chunk, &consumed,
            &new_file_block, &new_size, &allocated_blocks, tail_id,
            &tail_block, &tail_start);
        if (ret) {
            int rollback = infilfs_native_operation_rollback(pending);
            if (rollback && !ret)
                ret = rollback;
            break;
        }
        infilfs_native_operation_commit(pending);
        ii->object_block = new_file_block;
        i_size_write(inode, new_size);
        inode->i_blocks = allocated_blocks *
            (INFILFS_DISK_BLOCK_SIZE >> 9);
        pending->tx.next_sb.generation = cpu_to_le64(pending->tx.generation);
        sbi->disk = pending->tx.next_sb;
        infilfs_native_checksum_cache_store(
            inode->i_sb, ii->object_id, tail_id, tail_block, tail_start);
        done += consumed;
        pending->pending_bytes += consumed;

        if (pending->pending_bytes >= pending->publish_threshold) {
            int publish = infilfs_native_pending_commit_locked(pending);
            if (publish) {
                ret = publish;
                break;
            }
            if (done < requested) {
                ret = infilfs_native_pending_begin_locked(pending);
                if (ret)
                    break;
            }
        }
    }

    if (pending->active && !pending->commit_failed)
        mod_delayed_work(system_wq, &pending->idle_work,
                         INFILFS_NATIVE_IDLE_DELAY);
    if (done) {
        *position = pos + done;
        ret = (ssize_t)done;
    }
out_unlock:
    mutex_unlock(&sbi->write_lock);
    return ret;
}

static ssize_t infilfs_file_write_iter(struct kiocb *iocb,
                                       struct iov_iter *from)
{
    struct file *filep = iocb->ki_filp;
    struct inode *inode = file_inode(filep);
    struct address_space *mapping = inode->i_mapping;
    size_t requested = iov_iter_count(from);
    loff_t pos = iocb->ki_pos;
    loff_t end;
    ssize_t ret;

    if (pos < 0)
        return -EINVAL;
    if (!requested)
        return 0;
    if (filep->f_flags & O_APPEND)
        pos = i_size_read(inode);
    if (requested - 1u > LLONG_MAX - pos)
        return -EFBIG;
    end = pos + requested - 1u;

    /*
     * mmap uses the page cache while read(2)/write(2) retain the qualified
     * direct verified path.  Serialize direct writes against faults, publish
     * dirty mapped data first, then evict the affected cache range so a mapped
     * reader refaults the newly committed blocks.
     */
    filemap_invalidate_lock(mapping);
    ret = file_write_and_wait_range(filep, pos, end);
    if (ret)
        goto out_unlock;
    unmap_mapping_range(mapping, pos, requested, 0);
    ret = invalidate_inode_pages2_range(mapping, pos >> PAGE_SHIFT,
                                        end >> PAGE_SHIFT);
    if (ret)
        goto out_unlock;

    if ((u64)pos <= INFILFS_INLINE_DATA_MAX &&
        requested <= INFILFS_INLINE_DATA_MAX - (size_t)pos &&
        i_size_read(inode) <= INFILFS_INLINE_DATA_MAX) {
        ret = infilfs_native_pending_flush_sb(inode->i_sb);
        if (ret)
            goto out_unlock;
        iocb->ki_pos = pos;
        ret = infilfs_file_write_iter_legacy(iocb, from);
        if (ret != -EOPNOTSUPP)
            goto out_unlock;
    }

    iocb->ki_pos = pos;
    ret = infilfs_native_extent_write_iter(inode, &iocb->ki_pos, from,
                                            requested);
out_unlock:
    filemap_invalidate_unlock(mapping);
    return ret;
}

static int infilfs_file_fsync(struct file *file, loff_t start, loff_t end,
                              int datasync)
{
    int ret;

    (void)datasync;
    ret = file_write_and_wait_range(file, start, end);
    if (ret)
        return ret;
    return infilfs_native_pending_flush_sb(file_inode(file)->i_sb);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_rw_create(struct mnt_idmap *idmap, struct inode *dir,
                             struct dentry *dentry, umode_t mode, bool excl)
#else
static int infilfs_rw_create(struct user_namespace *idmap, struct inode *dir,
                             struct dentry *dentry, umode_t mode, bool excl)
#endif
{
    int ret = infilfs_native_pending_flush_sb(dir->i_sb);
    if (ret)
        return ret;
    return infilfs_rw_create_legacy(idmap, dir, dentry, mode, excl);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
static struct dentry *infilfs_rw_mkdir(struct mnt_idmap *idmap,
                                       struct inode *dir,
                                       struct dentry *dentry, umode_t mode)
{
    int ret = infilfs_native_pending_flush_sb(dir->i_sb);
    if (ret)
        return ERR_PTR(ret);
    return infilfs_rw_mkdir_legacy(idmap, dir, dentry, mode);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_rw_mkdir(struct mnt_idmap *idmap, struct inode *dir,
                            struct dentry *dentry, umode_t mode)
{
    int ret = infilfs_native_pending_flush_sb(dir->i_sb);
    if (ret)
        return ret;
    return infilfs_rw_mkdir_legacy(idmap, dir, dentry, mode);
}
#else
static int infilfs_rw_mkdir(struct user_namespace *idmap, struct inode *dir,
                            struct dentry *dentry, umode_t mode)
{
    int ret = infilfs_native_pending_flush_sb(dir->i_sb);
    if (ret)
        return ret;
    return infilfs_rw_mkdir_legacy(idmap, dir, dentry, mode);
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_rw_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
                              struct iattr *attr)
#else
static int infilfs_rw_setattr(struct user_namespace *idmap,
                              struct dentry *dentry, struct iattr *attr)
#endif
{
    int ret = infilfs_native_pending_flush_sb(d_inode(dentry)->i_sb);
    if (ret)
        return ret;
    return infilfs_rw_setattr_legacy(idmap, dentry, attr);
}

static void infilfs_rw_mount_destroy(struct super_block *sb)
{
    infilfs_native_pending_destroy(sb);
    infilfs_rw_mount_destroy_legacy(sb);
}

#define INFILFS_NATIVE_READ_INTEGRITY_PARITY 1
struct infilfs_native_read_checksum_cursor {
    u8 object_id[16];
    u64 object_block;
    u64 start_logical;
    bool valid;
};

static int infilfs_native_read_expected_digest(
    struct super_block *sb, const u8 owner_id[16], const u8 head_id[16],
    u64 logical, struct infilfs_native_read_checksum_cursor *cursor,
    u8 checksum_object[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_data_checksum_disk *expected)
{
    const u64 per = INFILFS_NATIVE_CHECKSUMS_PER_OBJECT;
    u64 target = (logical / per) * per;
    u8 current_id[16];
    u64 current_block = 0, previous_start = 0;
    bool have_previous = false;
    unsigned int guard;

    if (!memcmp(head_id, (u8[16]){0}, 16))
        return -EFSCORRUPTED;
    if (cursor->valid && cursor->start_logical <= target) {
        memcpy(current_id, cursor->object_id, 16);
        current_block = cursor->object_block;
    } else {
        memcpy(current_id, head_id, 16);
    }
    for (guard = 0; guard < 1048576u; ++guard) {
        struct infilfs_native_checksum_payload_disk *payload;
        struct infilfs_data_checksum_disk *values;
        u64 start, offset;
        u32 count;
        u16 type;
        int ret;
        if (!current_block) {
            ret = infilfs_index_lookup(sb, current_id, &current_block, &type);
            if (ret)
                return ret;
            if (type != INFILFS_OBJECT_CHECKSUM)
                return -EFSCORRUPTED;
        }
        ret = infilfs_native_checksum_decode(sb, owner_id, current_id,
                                              current_block, checksum_object,
                                              &payload);
        if (ret)
            return ret;
        start = le64_to_cpu(payload->start_logical_block);
        if ((start % per) != 0 || (have_previous && start <= previous_start) || start > target)
            return -EFSCORRUPTED;
        if (start == target) {
            count = le32_to_cpu(payload->checksum_count);
            offset = logical - start;
            if (offset >= count)
                return -EFSCORRUPTED;
            values = (struct infilfs_data_checksum_disk *)(payload + 1);
            *expected = values[offset];
            memcpy(cursor->object_id, current_id, 16);
            cursor->object_block = current_block;
            cursor->start_logical = start;
            cursor->valid = true;
            return 0;
        }
        if (!memcmp(payload->next_object_id, (u8[16]){0}, 16))
            return -EFSCORRUPTED;
        previous_start = start;
        have_previous = true;
        memcpy(current_id, payload->next_object_id, 16);
        current_block = 0;
    }
    return -ELOOP;
}

static __maybe_unused ssize_t infilfs_file_read_iter_verified(struct kiocb *iocb,
                                                struct iov_iter *to)
{
    struct file *filep = iocb->ki_filp;
    struct inode *inode = file_inode(filep);
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    const struct infilfs_object_header_disk *header;
    const struct infilfs_file_payload_disk *file;
    struct infilfs_native_read_checksum_cursor cursor = {0};
    loff_t pos = iocb->ki_pos;
    u64 file_size;
    size_t requested, done = 0;
    u8 *object = NULL, *data_block = NULL, *checksum_object = NULL;
    int ret;

    if (pos < 0)
        return -EINVAL;
    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    data_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    checksum_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!object || !data_block || !checksum_object) {
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
    if (le32_to_cpu(file->data_checksum_type) != INFILFS_CHECKSUM_SHA256) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    if ((u64)pos >= file_size) {
        ret = 0;
        goto out;
    }
    requested = min_t(u64, iov_iter_count(to), file_size - (u64)pos);

    if (le32_to_cpu(file->extent_count) == 0 && file_size <= INFILFS_INLINE_DATA_MAX) {
        const struct infilfs_data_checksum_disk *expected;
        const u8 *inline_bytes;
        u8 actual[32];
        size_t need, copied;
        if (!file_size) {
            ret = 0;
            goto out;
        }
        expected = (const struct infilfs_data_checksum_disk *)(file + 1);
        inline_bytes = (const u8 *)(expected + 1);
        need = sizeof(*file) + sizeof(*expected) + (size_t)file_size;
        if (need > le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        ret = infilfs_rw_inline_digest(inline_bytes, (size_t)file_size, actual);
        if (ret || memcmp(expected->bytes, actual, sizeof(actual)) != 0) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        copied = copy_to_iter(inline_bytes + pos, requested, to);
        iocb->ki_pos += copied;
        ret = copied;
        goto out;
    }

    while (done < requested) {
        u64 absolute = (u64)pos + done;
        u64 logical = absolute >> INFILFS_DISK_BLOCK_SHIFT;
        size_t within = absolute & (INFILFS_DISK_BLOCK_SIZE - 1u);
        size_t chunk = min_t(size_t, INFILFS_DISK_BLOCK_SIZE - within, requested - done);
        u64 physical = 0;
        u32 flags = 0;
        size_t copied;
        ret = infilfs_map_file_block(inode, object, logical, &physical, &flags);
        if (ret)
            goto partial;
        if (flags == INFILFS_EXTENT_HOLE) {
            memset(data_block, 0, INFILFS_DISK_BLOCK_SIZE);
        } else if (flags == INFILFS_EXTENT_NORMAL) {
            struct infilfs_data_checksum_disk expected, actual;
            ret = infilfs_read_block(inode->i_sb, physical, data_block);
            if (ret)
                goto partial;
            ret = infilfs_native_read_expected_digest(inode->i_sb, ii->object_id,
                file->checksum_head_id, logical, &cursor, checksum_object, &expected);
            if (ret)
                goto partial;
            infilfs_native_block_digest(data_block, &actual);
            if (memcmp(&expected, &actual, sizeof(expected)) != 0) {
                ret = -EFSCORRUPTED;
                goto partial;
            }
        } else {
            ret = -EFSCORRUPTED;
            goto partial;
        }
        copied = copy_to_iter(data_block + within, chunk, to);
        done += copied;
        if (copied != chunk)
            break;
    }
    iocb->ki_pos += done;
    ret = done;
    goto out;
partial:
    if (done) {
        iocb->ki_pos += done;
        ret = done;
    }
out:
    kfree(checksum_object);
    kfree(data_block);
    kfree(object);
    return ret;
}

static int infilfs_sync_fs(struct super_block *sb, int wait)
{
    return wait ? infilfs_native_pending_flush_sb(sb) : 0;
}

#define infilfs_file_read_iter infilfs_file_read_iter_verified
