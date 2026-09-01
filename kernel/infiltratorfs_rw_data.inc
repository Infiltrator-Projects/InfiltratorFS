// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Native Linux extent-backed data path for Format 0.17.
 *
 * The original RW bring-up deliberately stopped at inline files.  This layer
 * keeps that proven implementation as the small-file path and adds a bounded,
 * deferred native transaction for extent-backed sequential and random writes.
 * Committed data blocks are never overwritten in place; replacement and
 * reclamation consult both live ownership and the retained-snapshot union.
 *
 * Continuous writers share one transaction per mounted superblock.  The
 * working object-index/root pointers are visible to the live mount while the
 * committed allocation-map tree state remains untouched.  Publication occurs at the same
 * adaptive 16 MiB..512 MiB bound used by the native writer, on fsync, five
 * seconds after the writer becomes idle, or during unmount.  Transaction-
 * private metadata blocks are safely reused between syscalls; a small undo log
 * plus a transaction-local in-memory allocation-bit snapshot gives each write
 * syscall operation-level rollback.
 */

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define INFILFS_NATIVE_WRITE_CHUNK (4u * 1024u * 1024u)
#define INFILFS_NATIVE_METADATA_RESERVE_EXTRA_BLOCKS \
    (INFILFS_NATIVE_WRITE_CHUNK >> INFILFS_DISK_BLOCK_SHIFT)
#define INFILFS_NATIVE_IDLE_DELAY (5u * HZ)
#define INFILFS_NATIVE_UNDO_INITIAL 16u
#define INFILFS_NATIVE_CHECKSUM_CACHE_SLOTS 256u
#define INFILFS_NATIVE_CHECKSUM_GROUP_CACHE_SLOTS 4096u
#define INFILFS_NATIVE_WRITER_TAIL_SLOTS 64u
#define INFILFS_NATIVE_INDEX_LOCATOR_MIN_CAPACITY 64u
#define INFILFS_NATIVE_DIRECTORY_LOCATOR_MIN_CAPACITY 64u

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
    bool remove;
    bool found;
};

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
    u32 hash;
    u16 name_len;
    bool valid;
    u8 name[INFILFS_NAME_MAX];
};

struct infilfs_native_pending {
    struct list_head node;
    struct super_block *sb;
    struct infilfs_rw_tx tx;
    struct infilfs_superblock_disk base_disk;
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
    struct infilfs_native_writer_tail
        writer_tail[INFILFS_NATIVE_WRITER_TAIL_SLOTS];
    struct infilfs_native_index_locator *index_locators;
    u32 index_locator_capacity;
    u32 index_locator_count;
    bool index_locator_valid;
    struct infilfs_native_directory_locator *directory_locators;
    u8 directory_locator_owner_id[16];
    u32 directory_locator_capacity;
    u32 directory_locator_count;
    bool directory_locator_valid;
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
static struct infilfs_native_checksum_cache_entry
    infilfs_native_checksum_group_cache[
        INFILFS_NATIVE_CHECKSUM_GROUP_CACHE_SLOTS];

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

static enum infilfs_data_workload infilfs_native_classify_write(
    u64 pos, size_t bytes, u64 old_size)
{
    if (!bytes)
        return INFILFS_DATA_WORKLOAD_RANDOM;
    if (pos == old_size)
        return INFILFS_DATA_WORKLOAD_SEQUENTIAL;
    if (pos > old_size)
        return INFILFS_DATA_WORKLOAD_SPARSE;
    return INFILFS_DATA_WORKLOAD_RANDOM;
}

static u64 infilfs_native_block_distance(u64 a, u64 b)
{
    return a > b ? a - b : b - a;
}

static void infilfs_native_media_scores(
    const struct infilfs_sb_info *sbi, enum infilfs_data_workload workload,
    u64 distance, u64 slack, u64 tail,
    u64 *primary, u64 *secondary)
{
    if (sbi->media_profile == INFILFS_MEDIA_ROTATIONAL) {
        /*
         * HDDs pay for head movement: physical distance is the first-order
         * cost for every workload. Sequential ties preserve the longest tail;
         * random/sparse ties consume the tighter hole.
         */
        *primary = distance;
        *secondary = workload == INFILFS_DATA_WORKLOAD_SEQUENTIAL ?
            U64_MAX - tail : slack;
        return;
    }
    if (sbi->media_profile == INFILFS_MEDIA_NONROTATIONAL) {
        /*
         * Flash/NVMe has negligible seek cost. Streaming growth therefore
         * preserves the largest contiguous tail, while random/sparse CoW
         * consumes the tightest suitable hole before considering locality.
         */
        *primary = workload == INFILFS_DATA_WORKLOAD_SEQUENTIAL ?
            U64_MAX - tail : slack;
        *secondary = distance;
        return;
    }

    /* Balanced/unknown keeps the pre-media-aware workload policy. */
    *primary = workload == INFILFS_DATA_WORKLOAD_SEQUENTIAL ?
        distance : slack;
    *secondary = workload == INFILFS_DATA_WORKLOAD_SEQUENTIAL ?
        U64_MAX - tail : distance;
}


static void infilfs_native_writer_tail_invalidate(
    struct infilfs_native_pending *pending)
{
    memset(pending->writer_tail, 0, sizeof(pending->writer_tail));
}

static bool infilfs_native_writer_tail_lookup(
    struct infilfs_native_pending *pending, const u8 owner_id[16],
    struct infilfs_native_writer_tail *out)
{
    u32 slot = infilfs_native_id_hash(owner_id) &
        (INFILFS_NATIVE_WRITER_TAIL_SLOTS - 1u);
    struct infilfs_native_writer_tail *entry = &pending->writer_tail[slot];

    if (!entry->valid || memcmp(entry->owner_id, owner_id, 16) != 0)
        return false;
    if (out)
        *out = *entry;
    return true;
}

static void infilfs_native_writer_tail_store(
    struct infilfs_native_pending *pending, const u8 owner_id[16],
    const u8 object_id[16], u64 object_block, u64 start_logical)
{
    u32 slot = infilfs_native_id_hash(owner_id) &
        (INFILFS_NATIVE_WRITER_TAIL_SLOTS - 1u);
    struct infilfs_native_writer_tail *entry = &pending->writer_tail[slot];

    memset(entry, 0, sizeof(*entry));
    memcpy(entry->owner_id, owner_id, 16);
    memcpy(entry->object_id, object_id, 16);
    entry->object_block = object_block;
    entry->start_logical = start_logical;
    entry->valid = true;
}

static void infilfs_native_index_locator_invalidate(
    struct infilfs_native_pending *pending)
{
    pending->index_locator_valid = false;
}

static u32 infilfs_native_name_hash(const char *name, size_t length)
{
    u32 h = 2166136261u;
    size_t i;

    for (i = 0; i < length; ++i) {
        h ^= (u8)name[i];
        h *= 16777619u;
    }
    return h;
}

static void infilfs_native_directory_locator_invalidate(
    struct infilfs_native_pending *pending)
{
    pending->directory_locator_valid = false;
    pending->directory_locator_count = 0;
    memset(pending->directory_locator_owner_id, 0,
           sizeof(pending->directory_locator_owner_id));
}

static int infilfs_native_directory_locator_resize(
    struct infilfs_native_pending *pending, u32 requested)
{
    struct infilfs_native_directory_locator *old = pending->directory_locators;
    struct infilfs_native_directory_locator *fresh;
    u32 old_capacity = pending->directory_locator_capacity;
    u32 capacity = INFILFS_NATIVE_DIRECTORY_LOCATOR_MIN_CAPACITY;
    u32 i;

    while (capacity < requested) {
        if (capacity > U32_MAX / 2u)
            return -EOVERFLOW;
        capacity <<= 1;
    }
    if (capacity == old_capacity)
        return 0;

    fresh = kvmalloc_array(capacity, sizeof(*fresh), GFP_NOFS | __GFP_ZERO);
    if (!fresh)
        return -ENOMEM;

    for (i = 0; i < old_capacity; ++i) {
        u32 slot;
        u32 probes;

        if (!old || !old[i].valid)
            continue;
        slot = old[i].hash & (capacity - 1u);
        for (probes = 0; probes < capacity; ++probes) {
            struct infilfs_native_directory_locator *entry =
                &fresh[(slot + probes) & (capacity - 1u)];

            if (entry->valid)
                continue;
            *entry = old[i];
            break;
        }
        if (probes == capacity) {
            kvfree(fresh);
            return -ENOSPC;
        }
    }

    kvfree(old);
    pending->directory_locators = fresh;
    pending->directory_locator_capacity = capacity;
    return 0;
}

static int infilfs_native_directory_locator_ensure(
    struct infilfs_native_pending *pending, u32 wanted)
{
    u64 needed;

    if (wanted < INFILFS_NATIVE_DIRECTORY_LOCATOR_MIN_CAPACITY / 2u)
        wanted = INFILFS_NATIVE_DIRECTORY_LOCATOR_MIN_CAPACITY / 2u;
    needed = (u64)wanted * 2u;
    if (needed > U32_MAX)
        return -EOVERFLOW;
    if (pending->directory_locator_capacity >= (u32)needed)
        return 0;
    return infilfs_native_directory_locator_resize(pending, (u32)needed);
}

static bool infilfs_native_directory_locator_matches(
    struct infilfs_native_pending *pending, const u8 owner_id[16], u32 count)
{
    return pending->directory_locator_valid &&
        pending->directory_locator_count == count &&
        memcmp(pending->directory_locator_owner_id, owner_id, 16) == 0;
}

static bool infilfs_native_directory_locator_lookup(
    struct infilfs_native_pending *pending, const u8 owner_id[16],
    const char *name, size_t name_len)
{
    u32 hash;
    u32 slot;
    u32 probes;

    if (!name || name_len > INFILFS_NAME_MAX ||
        !pending->directory_locator_valid ||
        memcmp(pending->directory_locator_owner_id, owner_id, 16) != 0 ||
        !pending->directory_locator_capacity)
        return false;

    hash = infilfs_native_name_hash(name, name_len);
    slot = hash & (pending->directory_locator_capacity - 1u);
    for (probes = 0; probes < pending->directory_locator_capacity; ++probes) {
        struct infilfs_native_directory_locator *entry =
            &pending->directory_locators[
                (slot + probes) & (pending->directory_locator_capacity - 1u)];

        if (!entry->valid)
            return false;
        if (entry->hash == hash && entry->name_len == name_len &&
            memcmp(entry->name, name, name_len) == 0)
            return true;
    }
    return false;
}

static int infilfs_native_directory_locator_insert(
    struct infilfs_native_pending *pending, const u8 owner_id[16],
    const char *name, size_t name_len)
{
    struct infilfs_native_directory_locator *entry;
    u32 hash;
    u32 slot;
    u32 probes;
    int ret;

    if (!name || !name_len || name_len > INFILFS_NAME_MAX)
        return -EINVAL;
    if (memcmp(pending->directory_locator_owner_id, owner_id, 16) != 0)
        return -ESTALE;
    if (((u64)pending->directory_locator_count + 1u) * 10u >=
        (u64)pending->directory_locator_capacity * 7u) {
        ret = infilfs_native_directory_locator_ensure(
            pending, pending->directory_locator_count + 1u);
        if (ret)
            return ret;
    }
    if (!pending->directory_locator_capacity) {
        ret = infilfs_native_directory_locator_ensure(pending, 1u);
        if (ret)
            return ret;
    }

    hash = infilfs_native_name_hash(name, name_len);
    slot = hash & (pending->directory_locator_capacity - 1u);
    for (probes = 0; probes < pending->directory_locator_capacity; ++probes) {
        entry = &pending->directory_locators[
            (slot + probes) & (pending->directory_locator_capacity - 1u)];
        if (entry->valid) {
            if (entry->hash == hash && entry->name_len == name_len &&
                memcmp(entry->name, name, name_len) == 0)
                return -EEXIST;
            continue;
        }
        memset(entry, 0, sizeof(*entry));
        entry->hash = hash;
        entry->name_len = (u16)name_len;
        memcpy(entry->name, name, name_len);
        entry->valid = true;
        pending->directory_locator_count++;
        return 0;
    }
    return -ENOSPC;
}

static int infilfs_native_index_locator_resize(
    struct infilfs_native_pending *pending, u32 requested)
{
    struct infilfs_native_index_locator *old = pending->index_locators;
    struct infilfs_native_index_locator *fresh;
    u32 old_capacity = pending->index_locator_capacity;
    u32 capacity = INFILFS_NATIVE_INDEX_LOCATOR_MIN_CAPACITY;
    u32 i;

    while (capacity < requested) {
        if (capacity > U32_MAX / 2u)
            return -EOVERFLOW;
        capacity <<= 1;
    }
    if (capacity == old_capacity)
        return 0;

    fresh = kvmalloc_array(capacity, sizeof(*fresh), GFP_NOFS | __GFP_ZERO);
    if (!fresh)
        return -ENOMEM;

    for (i = 0; i < old_capacity; ++i) {
        u32 slot;
        u32 probes;

        if (!old || !old[i].valid)
            continue;
        slot = infilfs_native_id_hash(old[i].object_id) & (capacity - 1u);
        for (probes = 0; probes < capacity; ++probes) {
            struct infilfs_native_index_locator *entry =
                &fresh[(slot + probes) & (capacity - 1u)];

            if (entry->valid)
                continue;
            *entry = old[i];
            break;
        }
        if (probes == capacity) {
            kvfree(fresh);
            return -ENOSPC;
        }
    }

    kvfree(old);
    pending->index_locators = fresh;
    pending->index_locator_capacity = capacity;
    return 0;
}

static int infilfs_native_index_locator_ensure(
    struct infilfs_native_pending *pending, u32 wanted)
{
    u64 needed;

    if (wanted < INFILFS_NATIVE_INDEX_LOCATOR_MIN_CAPACITY / 2u)
        wanted = INFILFS_NATIVE_INDEX_LOCATOR_MIN_CAPACITY / 2u;
    needed = (u64)wanted * 2u;
    if (needed > U32_MAX)
        return -EOVERFLOW;
    if (pending->index_locator_capacity >= (u32)needed)
        return 0;
    return infilfs_native_index_locator_resize(pending, (u32)needed);
}

static int infilfs_native_index_locator_insert(
    struct infilfs_native_pending *pending, const u8 object_id[16],
    u32 page_index, u32 entry_index)
{
    struct infilfs_native_index_locator *entry;
    u32 slot;
    u32 probes;
    int ret;

    if ((u64)(pending->index_locator_count + 1u) * 10u >=
        (u64)pending->index_locator_capacity * 7u) {
        ret = infilfs_native_index_locator_ensure(
            pending, pending->index_locator_count + 1u);
        if (ret)
            return ret;
    }
    if (!pending->index_locator_capacity) {
        ret = infilfs_native_index_locator_ensure(pending, 1u);
        if (ret)
            return ret;
    }

    slot = infilfs_native_id_hash(object_id) &
        (pending->index_locator_capacity - 1u);
    for (probes = 0; probes < pending->index_locator_capacity; ++probes) {
        entry = &pending->index_locators[
            (slot + probes) & (pending->index_locator_capacity - 1u)];
        if (entry->valid) {
            if (memcmp(entry->object_id, object_id, 16) != 0)
                continue;
            entry->page_index = page_index;
            entry->entry_index = entry_index;
            return 0;
        }
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->object_id, object_id, 16);
        entry->page_index = page_index;
        entry->entry_index = entry_index;
        entry->valid = true;
        pending->index_locator_count++;
        return 0;
    }
    return -ENOSPC;
}

static bool infilfs_native_index_locator_lookup(
    struct infilfs_native_pending *pending, const u8 object_id[16],
    u32 *page_index, u32 *entry_index)
{
    u32 slot;
    u32 probes;

    if (!pending->index_locator_valid || !pending->index_locator_capacity)
        return false;
    slot = infilfs_native_id_hash(object_id) &
        (pending->index_locator_capacity - 1u);
    for (probes = 0; probes < pending->index_locator_capacity; ++probes) {
        struct infilfs_native_index_locator *entry =
            &pending->index_locators[
                (slot + probes) & (pending->index_locator_capacity - 1u)];

        if (!entry->valid)
            return false;
        if (memcmp(entry->object_id, object_id, 16) != 0)
            continue;
        if (page_index)
            *page_index = entry->page_index;
        if (entry_index)
            *entry_index = entry->entry_index;
        return true;
    }
    return false;
}

static int infilfs_native_index_locator_build(
    struct infilfs_native_pending *pending,
    const u8 head[INFILFS_DISK_BLOCK_SIZE])
{
    const struct infilfs_object_header_disk *header =
        (const struct infilfs_object_header_disk *)head;
    const struct infilfs_index_payload_disk *payload =
        (const struct infilfs_index_payload_disk *)(header + 1);
    const __le64 *pages = (const __le64 *)(payload + 1);
    u32 page_count = le32_to_cpu(payload->reserved);
    u32 total_count = le32_to_cpu(payload->entry_count);
    u8 *page_block = NULL;
    u32 seen = 0;
    u32 p;
    int ret;

    if (le16_to_cpu(header->object_version) != INFILFS_OBJECT_VERSION_PAGED)
        return -EOPNOTSUPP;
    if ((total_count && !page_count) ||
        page_count > INFILFS_INDEX_PAGE_POINTERS)
        return -EFSCORRUPTED;

    ret = infilfs_native_index_locator_ensure(pending, total_count + 32u);
    if (ret)
        return ret;
    memset(pending->index_locators, 0,
           (size_t)pending->index_locator_capacity *
               sizeof(*pending->index_locators));
    pending->index_locator_count = 0;
    pending->index_locator_valid = false;

    page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!page_block)
        return -ENOMEM;
    pending->index_locator_valid = true;

    for (p = 0; p < page_count; ++p) {
        struct infilfs_metadata_page_disk *page;
        struct infilfs_index_entry_disk *entries;
        u32 count;
        u32 i;

        ret = infilfs_read_allocated_block(
            pending->sb, le64_to_cpu(pages[p]), page_block);
        if (ret)
            goto out;
        if (!infilfs_metadata_page_valid(
                pending->sb, page_block, infilfs_index_page_magic,
                header->object_id)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        page = (struct infilfs_metadata_page_disk *)page_block;
        count = le32_to_cpu(page->entry_count);
        if (count > INFILFS_INDEX_ENTRIES_PER_PAGE ||
            le32_to_cpu(page->bytes_used) !=
                count * sizeof(struct infilfs_index_entry_disk) ||
            seen > total_count || count > total_count - seen) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        entries = (struct infilfs_index_entry_disk *)(page + 1);
        for (i = 0; i < count; ++i) {
            if (infilfs_native_index_locator_lookup(
                    pending, entries[i].object_id, NULL, NULL)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            ret = infilfs_native_index_locator_insert(
                pending, entries[i].object_id, p, i);
            if (ret)
                goto out;
        }
        seen += count;
    }
    if (seen != total_count) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    pending->index_locator_valid = true;
    ret = 0;
out:
    kfree(page_block);
    if (ret)
        pending->index_locator_valid = false;
    return ret;
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
    for (i = 0; i < INFILFS_NATIVE_CHECKSUM_GROUP_CACHE_SLOTS; ++i)
        if (infilfs_native_checksum_group_cache[i].valid &&
            infilfs_native_checksum_group_cache[i].sb == sb)
            infilfs_native_checksum_group_cache[i].valid = false;
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

static u32 infilfs_native_checksum_group_slot(const u8 owner_id[16],
                                              u64 start_logical)
{
    u32 h = infilfs_native_id_hash(owner_id);

    h ^= (u32)start_logical;
    h *= 16777619u;
    h ^= (u32)(start_logical >> 32);
    h *= 16777619u;
    return h & (INFILFS_NATIVE_CHECKSUM_GROUP_CACHE_SLOTS - 1u);
}

static bool infilfs_native_checksum_group_cache_lookup(
    struct super_block *sb, const u8 owner_id[16], u64 start_logical,
    struct infilfs_native_checksum_cache_entry *out)
{
    unsigned long flags;
    u32 slot = infilfs_native_checksum_group_slot(owner_id, start_logical);
    bool found = false;

    spin_lock_irqsave(&infilfs_native_checksum_cache_lock, flags);
    if (infilfs_native_checksum_group_cache[slot].valid &&
        infilfs_native_checksum_group_cache[slot].sb == sb &&
        infilfs_native_checksum_group_cache[slot].start_logical ==
            start_logical &&
        memcmp(infilfs_native_checksum_group_cache[slot].owner_id,
               owner_id, 16) == 0) {
        if (out)
            *out = infilfs_native_checksum_group_cache[slot];
        found = true;
    }
    spin_unlock_irqrestore(&infilfs_native_checksum_cache_lock, flags);
    return found;
}

static void infilfs_native_checksum_group_cache_store(
    struct super_block *sb, const u8 owner_id[16], const u8 object_id[16],
    u64 object_block, u64 start_logical)
{
    unsigned long flags;
    u32 slot = infilfs_native_checksum_group_slot(owner_id, start_logical);
    struct infilfs_native_checksum_cache_entry *entry;

    spin_lock_irqsave(&infilfs_native_checksum_cache_lock, flags);
    entry = &infilfs_native_checksum_group_cache[slot];
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
    struct infilfs_native_undo *grown;
    unsigned int new_capacity;
    unsigned int i;
    int ret;

    for (i = 0; i < pending->undo_count; ++i)
        if (pending->undo[i].block == block)
            return 0;

    if (pending->undo_count == pending->undo_capacity) {
        new_capacity = pending->undo_capacity ?
            pending->undo_capacity * 2u : INFILFS_NATIVE_UNDO_INITIAL;
        if (new_capacity < pending->undo_capacity ||
            (size_t)new_capacity > SIZE_MAX / sizeof(*grown))
            return -EOVERFLOW;
        grown = kvmalloc_array(new_capacity, sizeof(*grown), GFP_NOFS);
        if (!grown)
            return -ENOMEM;
        if (pending->undo_count)
            memcpy(grown, pending->undo,
                   (size_t)pending->undo_count * sizeof(*grown));
        kvfree(pending->undo);
        pending->undo = grown;
        pending->undo_capacity = new_capacity;
    }

    ret = infilfs_read_allocated_block(pending->sb, block,
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

/*
 * Keep enough currently free blocks unavailable to ordinary file-data
 * allocation to guarantee that the current transaction can still publish its
 * allocation tree and finish bounded metadata CoW work. Internal metadata
 * allocation uses the ordinary transaction allocator and can consume this
 * reserve; only user data is prevented from exhausting it.
 */
static u64 infilfs_native_metadata_reserve_blocks(
    const struct infilfs_sb_info *sbi)
{
    u64 reserve;
    u64 total;

    if (!sbi)
        return 0;
    total = sbi->device_blocks;
    reserve = (u64)sbi->allocation_leaf_count +
        (u64)sbi->allocation_branch_count;
    if (reserve > U64_MAX - INFILFS_NATIVE_METADATA_RESERVE_EXTRA_BLOCKS)
        return total;
    reserve += INFILFS_NATIVE_METADATA_RESERVE_EXTRA_BLOCKS;
    return min_t(u64, reserve, total);
}

/*
 * statfs must describe the active deferred transaction, not just the last
 * committed checkpoint. The pending-list lock protects object lifetime; the
 * free-block scalar itself is sampled with READ_ONCE because writers serialize
 * updates under sbi->write_lock.
 */
static u64 infilfs_native_visible_free_blocks(struct super_block *sb)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct infilfs_native_pending *pending;
    u64 free_blocks;

    if (!sbi)
        return 0;
    free_blocks = le64_to_cpu(READ_ONCE(sbi->disk.free_blocks));

    mutex_lock(&infilfs_native_pending_list_lock);
    pending = infilfs_native_pending_find_locked(sb);
    if (pending && READ_ONCE(pending->active))
        free_blocks = READ_ONCE(pending->tx.free_blocks);
    mutex_unlock(&infilfs_native_pending_list_lock);
    return free_blocks;
}

static u64 infilfs_native_available_data_blocks(struct super_block *sb)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    u64 free_blocks = infilfs_native_visible_free_blocks(sb);
    u64 reserve = infilfs_native_metadata_reserve_blocks(sbi);

    return free_blocks > reserve ? free_blocks - reserve : 0;
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
        const u64 max_publish = 512ULL * 1024ULL * 1024ULL;

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
    pending->operation_allocated_count = pending->tx.allocated_count;
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
    size_t a;
    unsigned int i;
    int first_error = 0;

    for (i = 0; i < pending->undo_count; ++i) {
        int ret = infilfs_native_stage_block(pending->sb,
                                             pending->undo[i].block,
                                             pending->undo[i].data);
        if (ret && !first_error)
            first_error = ret;
    }

    /*
     * All frees are deferred until publication, so an operation can only have
     * changed the working bitmap by allocating new ranges.  Clear precisely
     * those ranges instead of copying the complete multi-megabyte bitmap back.
     */
    write_lock(&sbi->bitmap_lock);
    for (a = pending->operation_allocated_count;
         a < pending->tx.allocated_count; ++a) {
        u64 start = pending->tx.allocated[a].start;
        u64 count = pending->tx.allocated[a].count;
        u64 b;

        for (b = 0; b < count; ++b)
            infilfs_rw_bitmap_set(pending->tx.bitmap, start + b, false);
    }
    write_unlock(&sbi->bitmap_lock);
    pending->tx.allocated_count = pending->operation_allocated_count;
    pending->tx.next_sb = pending->operation_next_sb;
    pending->tx.free_blocks = pending->operation_free_blocks;
    (void)infilfs_rw_free_extent_index_rebuild(&pending->tx);
    pending->tx.deferred_count = pending->operation_deferred_count;
    if (pending->operation_had_last && pending->tx.deferred_count)
        pending->tx.deferred[pending->tx.deferred_count - 1u] =
            pending->operation_last_deferred;
    sbi->disk = pending->operation_next_sb;
    pending->undo_count = 0;
    infilfs_native_writer_tail_invalidate(pending);
    infilfs_native_index_locator_invalidate(pending);
    infilfs_native_directory_locator_invalidate(pending);
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
    kvfree(pending->undo);
    kvfree(pending->index_locators);
    kvfree(pending->directory_locators);
    kfree(pending);
}

static bool infilfs_native_choose_scored_extent(
    const struct infilfs_rw_tx *tx, u64 wanted, u64 preferred,
    enum infilfs_data_workload workload, u64 *start_out)
{
    const struct infilfs_sb_info *sbi;
    u64 best_primary = U64_MAX;
    u64 best_secondary = U64_MAX;
    u64 best_start = 0;
    bool found = false;
    size_t i;

    if (!tx || !tx->sbi || !tx->free_extent_index_valid || !wanted ||
        !tx->free_extent_count || !start_out)
        return false;
    sbi = tx->sbi;
    if (preferred < 1u || preferred >= sbi->device_blocks) {
        preferred = sbi->data_alloc_hint;
        if (preferred < 1u || preferred >= sbi->device_blocks)
            preferred = 1u;
    }

    for (i = 0; i < tx->free_extent_count; ++i) {
        const struct infilfs_rw_free_range *extent = &tx->free_extents[i];
        u64 extent_start = extent->start;
        u64 extent_end;
        u64 candidate;
        u64 distance;
        u64 slack;
        u64 tail;
        u64 primary;
        u64 secondary;

        if (extent->count < wanted ||
            extent_start > U64_MAX - extent->count)
            continue;
        extent_end = extent_start + extent->count;

        if (workload == INFILFS_DATA_WORKLOAD_SEQUENTIAL) {
            if (preferred <= extent_start)
                candidate = extent_start;
            else if (preferred <= extent_end - wanted)
                candidate = preferred;
            else
                candidate = extent_end - wanted;
        } else {
            if (preferred <= extent_start)
                candidate = extent_start;
            else if (preferred >= extent_end - wanted)
                candidate = extent_end - wanted;
            else
                candidate = preferred;
        }

        distance = infilfs_native_block_distance(candidate, preferred);
        slack = extent->count - wanted;
        tail = extent_end - (candidate + wanted);
        infilfs_native_media_scores(
            sbi, workload, distance, slack, tail, &primary, &secondary);

        if (infilfs_parallel_range_reserved(sbi, candidate, wanted))
            continue;
        if (!found || primary < best_primary ||
            (primary == best_primary && secondary < best_secondary) ||
            (primary == best_primary && secondary == best_secondary &&
             candidate < best_start)) {
            best_primary = primary;
            best_secondary = secondary;
            best_start = candidate;
            found = true;
        }
    }

    if (!found)
        return false;
    *start_out = best_start;
    atomic64_inc(&tx->sbi->allocation_locality_scored);
    switch (tx->sbi->media_profile) {
    case INFILFS_MEDIA_ROTATIONAL:
        atomic64_inc(&tx->sbi->allocation_media_rotational_scored);
        break;
    case INFILFS_MEDIA_NONROTATIONAL:
        atomic64_inc(&tx->sbi->allocation_media_nonrotational_scored);
        break;
    case INFILFS_MEDIA_BALANCED:
    default:
        atomic64_inc(&tx->sbi->allocation_media_balanced_scored);
        break;
    }
    if (workload != INFILFS_DATA_WORKLOAD_SEQUENTIAL &&
        tx->sbi->media_profile != INFILFS_MEDIA_ROTATIONAL)
        atomic64_inc(&tx->sbi->allocation_best_fit);
    return true;
}

static int infilfs_native_alloc_data_exact_reserved(
    struct infilfs_native_pending *pending, u64 count, u64 preferred,
    enum infilfs_data_workload workload,
    struct infilfs_parallel_reservation *reservation, u64 *start_out)
{
    struct infilfs_rw_tx *tx = &pending->tx;
    struct infilfs_sb_info *sbi = tx->sbi;
    u64 total = sbi->device_blocks;
    u64 hint;
    u64 candidate;
    u64 scanned;
    u64 start = 0;
    u64 run = 0;
    u64 i;
    unsigned int attempts = 0;
    int ret;

    infilfs_parallel_note_workload(sbi, workload);

retry:
    run = 0;
    start = 0;
    if (attempts++ >= INFILFS_ALLOCATION_RESERVATION_SHARDS)
        return -ENOSPC;
    if (!count || count > tx->free_blocks || total <= 1)
        return -ENOSPC;
    {
        u64 reserve = infilfs_native_metadata_reserve_blocks(sbi);

        if (tx->free_blocks <= reserve ||
            count > tx->free_blocks - reserve)
            return -ENOSPC;
    }

    if (workload == INFILFS_DATA_WORKLOAD_SEQUENTIAL &&
        reservation && reservation->active &&
        reservation->count == count) {
        ret = infilfs_parallel_consume_reservation(
            tx, reservation, count, start_out);
        if (!ret) {
            sbi->data_alloc_hint = *start_out + count < total ?
                *start_out + count : 1u;
            return 0;
        }
        if (ret != -ENOENT && ret != -EAGAIN)
            return ret;
    }

    if (workload == INFILFS_DATA_WORKLOAD_SEQUENTIAL &&
        preferred > 0 && preferred < total &&
        count <= total - preferred) {
        for (i = 0; i < count; ++i)
            if (infilfs_rw_bitmap_get(tx->bitmap, preferred + i))
                break;
        if (i == count &&
            !infilfs_parallel_range_reserved(sbi, preferred, count)) {
            ret = infilfs_rw_tx_record_alloc(tx, preferred, count);
            if (ret == -EAGAIN) {
                infilfs_rw_free_extent_index_invalidate(tx);
                goto retry;
            }
            if (ret)
                return ret;
            sbi->data_alloc_hint = preferred + count < total ?
                preferred + count : 1u;
            *start_out = preferred;
            return 0;
        }
    }

    /*
     * The old fallback restarted at block 1 for every random CoW overwrite.
     * Continue from the previous successful search and wrap once, making
     * repeated 4 KiB overwrites proportional to nearby occupancy rather than
     * to all allocations before the first free block.
     */
    hint = sbi->data_alloc_hint;
    if (hint < 1 || hint >= total)
        hint = 1;
    candidate = hint;

    if (tx->free_extent_index_valid &&
        infilfs_native_choose_scored_extent(
            tx, count, preferred, workload, &start)) {
        run = count;
    } else if (tx->free_extent_index_valid &&
               infilfs_rw_free_extent_choose_forward(
                   tx, count, hint, &start) &&
               !infilfs_parallel_range_reserved(sbi, start, count)) {
        run = count;
    } else {
        for (scanned = 0; scanned < total - 1u; ++scanned) {
            if (!infilfs_rw_bitmap_get(tx->bitmap, candidate) &&
                !infilfs_parallel_range_reserved(sbi, candidate, 1u)) {
                if (!run)
                    start = candidate;
                run++;
                if (run == count)
                    break;
            } else {
                run = 0;
            }

            candidate++;
            if (candidate >= total) {
                candidate = 1;
                run = 0;
            }
        }
    }
    if (run != count)
        return -ENOSPC;

    ret = infilfs_rw_tx_record_alloc(tx, start, count);
    if (ret == -EAGAIN) {
        infilfs_rw_free_extent_index_invalidate(tx);
        goto retry;
    }
    if (ret)
        return ret;
    sbi->data_alloc_hint = start + count < total ? start + count : 1u;
    *start_out = start;
    return 0;
}

static int infilfs_native_alloc_data_exact(
    struct infilfs_native_pending *pending, u64 count, u64 preferred,
    u64 *start_out)
{
    return infilfs_native_alloc_data_exact_reserved(
        pending, count, preferred, INFILFS_DATA_WORKLOAD_RANDOM,
        NULL, start_out);
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

            ret = infilfs_read_allocated_block(inode->i_sb, block, page_block);
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
            !infilfs_extent_flags_valid(blocks, physical, flags)) {
            ret = -EFSCORRUPTED;
            goto fail;
        }
        if (infilfs_extent_is_compressed(flags) &&
            (le64_to_cpu(INFILFS_SB(inode->i_sb)->disk.incompat_flags) &
             INFILFS_INCOMPAT_COMPRESSED_EXTENTS) == 0) {
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

static int infilfs_native_map_extent_detail(
    const struct infilfs_extent_disk *extents, u32 count, u64 logical,
    u64 *physical_out, u32 *flags_out,
    u64 *extent_start_out, u32 *extent_blocks_out)
{
    u32 i;

    for (i = 0; i < count; ++i) {
        u64 start = le64_to_cpu(extents[i].logical_block);
        u32 blocks = le32_to_cpu(extents[i].block_count);
        u32 flags = le32_to_cpu(extents[i].flags);

        if (logical < start || logical >= start + blocks)
            continue;
        *flags_out = flags;
        if (infilfs_extent_kind(flags) == INFILFS_EXTENT_HOLE)
            *physical_out = 0;
        else if (infilfs_extent_is_compressed(flags))
            *physical_out = le64_to_cpu(extents[i].physical_block);
        else
            *physical_out =
                le64_to_cpu(extents[i].physical_block) + logical - start;
        if (extent_start_out)
            *extent_start_out = start;
        if (extent_blocks_out)
            *extent_blocks_out = blocks;
        return 0;
    }
    return -ENOENT;
}

static int infilfs_native_map_extent(
    const struct infilfs_extent_disk *extents, u32 count, u64 logical,
    u64 *physical_out, u32 *flags_out)
{
    return infilfs_native_map_extent_detail(
        extents, count, logical, physical_out, flags_out, NULL, NULL);
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
        if (!infilfs_extent_is_compressed(flags) &&
            le64_to_cpu(last->logical_block) +
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
        if (infilfs_extent_is_compressed(flags) && keep_end != end) {
            ret = -EOPNOTSUPP;
            goto fail;
        }
        ret = infilfs_native_extent_append(out, capacity, &count, start,
            infilfs_extent_kind(flags) == INFILFS_EXTENT_NORMAL ?
                physical : 0,
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
        if (infilfs_extent_is_compressed(flags) && keep_start != start) {
            ret = -EOPNOTSUPP;
            goto fail;
        }
        ret = infilfs_native_extent_append(out, capacity, &count, keep_start,
            infilfs_extent_kind(flags) == INFILFS_EXTENT_NORMAL ?
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

        {
            u32 flags = le32_to_cpu(extents[i].flags);
            u32 logical_blocks = le32_to_cpu(extents[i].block_count);

            if (infilfs_extent_kind(flags) != INFILFS_EXTENT_NORMAL ||
                extent_end <= replace_start || logical >= replace_end)
                continue;
            first = max_t(u64, logical, replace_start);
            last = min_t(u64, extent_end, replace_end);
            if (infilfs_extent_is_compressed(flags)) {
                if (first != logical || last != extent_end)
                    return -EOPNOTSUPP;
                physical = le64_to_cpu(extents[i].physical_block);
                ret = infilfs_ns_free_unshared_run(
                    pending, owner_id, physical,
                    infilfs_extent_physical_blocks(logical_blocks, flags));
            } else {
                physical = le64_to_cpu(extents[i].physical_block) +
                    first - logical;
                ret = infilfs_ns_free_unshared_run(
                    pending, owner_id, physical, last - first);
            }
        }
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

static int infilfs_native_checksum_find_group(
    struct super_block *sb, const u8 owner_id[16], const u8 head_id[16],
    u64 group_start, struct infilfs_native_checksum_cache_entry *out)
{
    struct infilfs_native_checksum_cache_entry cached;
    struct infilfs_native_checksum_payload_disk *payload;
    u8 current_id[16];
    u8 *object;
    u64 previous_start = 0;
    bool have_previous = false;
    unsigned int guard;
    int ret;

    if (!memchr_inv(head_id, 0, 16))
        return -ENOENT;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;

    if (infilfs_native_checksum_group_cache_lookup(
            sb, owner_id, group_start, &cached)) {
        u64 indexed_block = 0;
        u16 indexed_type = 0;

        ret = infilfs_index_lookup(sb, cached.object_id, &indexed_block,
                                   &indexed_type);
        if (!ret && indexed_type == INFILFS_OBJECT_CHECKSUM &&
            indexed_block == cached.object_block) {
            ret = infilfs_native_checksum_decode(
                sb, owner_id, cached.object_id, cached.object_block,
                object, &payload);
            if (!ret &&
                le64_to_cpu(payload->start_logical_block) == group_start) {
                if (out)
                    *out = cached;
                kfree(object);
                return 0;
            }
        }
    }

    memcpy(current_id, head_id, sizeof(current_id));
    for (guard = 0; guard < 1048576u &&
         memchr_inv(current_id, 0, sizeof(current_id)); ++guard) {
        struct infilfs_native_checksum_cache_entry found;
        u8 next_id[16];
        u64 block;
        u64 start;
        u16 type;

        ret = infilfs_index_lookup(sb, current_id, &block, &type);
        if (ret)
            goto out;
        if (type != INFILFS_OBJECT_CHECKSUM) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        ret = infilfs_native_checksum_decode(
            sb, owner_id, current_id, block, object, &payload);
        if (ret)
            goto out;
        start = le64_to_cpu(payload->start_logical_block);
        if ((start % INFILFS_NATIVE_CHECKSUMS_PER_OBJECT) != 0 ||
            (have_previous && start <= previous_start)) {
            ret = -EFSCORRUPTED;
            goto out;
        }

        infilfs_native_checksum_group_cache_store(
            sb, owner_id, current_id, block, start);
        if (start == group_start) {
            memset(&found, 0, sizeof(found));
            found.sb = sb;
            memcpy(found.owner_id, owner_id, 16);
            memcpy(found.object_id, current_id, 16);
            found.object_block = block;
            found.start_logical = start;
            found.valid = true;
            if (out)
                *out = found;
            ret = 0;
            goto out;
        }
        if (start > group_start) {
            ret = -ENOENT;
            goto out;
        }

        memcpy(next_id, payload->next_object_id, sizeof(next_id));
        previous_start = start;
        have_previous = true;
        memcpy(current_id, next_id, sizeof(current_id));
    }
    ret = -ENOENT;
out:
    kfree(object);
    return ret;
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
static int infilfs_native_checksum_update_existing_group(
    struct infilfs_native_pending *pending,
    struct infilfs_file_payload_disk *file, const u8 owner_id[16],
    u64 touched_start, u64 touched_count,
    const struct infilfs_data_checksum_disk *digests,
    struct infilfs_native_index_change *changes, u32 change_capacity,
    u32 *change_count, u8 final_tail_id[16], u64 *final_tail_block,
    u64 *final_tail_start)
{
    const u64 per = INFILFS_NATIVE_CHECKSUMS_PER_OBJECT;
    struct infilfs_native_checksum_cache_entry target;
    struct infilfs_native_checksum_payload_disk *payload;
    struct infilfs_data_checksum_disk *values;
    u8 tail_id[16];
    u8 *tail_object = NULL;
    u8 *object = NULL;
    u64 tail_block = 0;
    u64 tail_start = 0;
    u64 group_start;
    u64 first;
    u64 last;
    u64 stored_block;
    u32 needed;
    int ret;

    if (!touched_count || touched_start > U64_MAX - touched_count)
        return -EINVAL;
    group_start = (touched_start / per) * per;
    if (((touched_start + touched_count - 1u) / per) * per != group_start)
        return -ENOENT;
    if (*change_count >= change_capacity)
        return -ENOSPC;

    /*
     * Preserve the authoritative tail before changing a checksum object.
     * In the steady state this is one validated cache lookup; if the cache was
     * lost, the general tail walker repopulates it once.
     */
    tail_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!tail_object || !object) {
        ret = -ENOMEM;
        goto out;
    }
    ret = infilfs_native_checksum_tail(
        pending->sb, owner_id, file->checksum_head_id,
        tail_id, &tail_block, &tail_start, tail_object);
    if (ret)
        goto out;

    ret = infilfs_native_checksum_find_group(
        pending->sb, owner_id, file->checksum_head_id,
        group_start, &target);
    if (ret)
        goto out;

    ret = infilfs_native_checksum_decode(
        pending->sb, owner_id, target.object_id,
        target.object_block, object, &payload);
    if (ret)
        goto out;
    if (le64_to_cpu(payload->start_logical_block) != group_start) {
        ret = -EFSCORRUPTED;
        goto out;
    }

    first = touched_start - group_start;
    last = first + touched_count;
    if (last > per) {
        ret = -EOVERFLOW;
        goto out;
    }
    needed = max_t(u32, le32_to_cpu(payload->checksum_count), (u32)last);
    values = (struct infilfs_data_checksum_disk *)(payload + 1);
    memcpy(values + first, digests,
           (size_t)touched_count * sizeof(*values));
    payload->checksum_count = cpu_to_le32(needed);
    ((struct infilfs_object_header_disk *)object)->payload_size =
        cpu_to_le32(sizeof(*payload) + (size_t)needed * sizeof(*values));
    ((struct infilfs_object_header_disk *)object)->generation =
        cpu_to_le64(pending->tx.generation);
    ret = infilfs_rw_finalize_object(object);
    if (ret)
        goto out;

    ret = infilfs_native_store_private_or_cow(
        pending, target.object_block, object, &stored_block);
    if (ret)
        goto out;

    memcpy(changes[*change_count].object_id, target.object_id, 16);
    changes[*change_count].object_block = stored_block;
    changes[*change_count].object_type = INFILFS_OBJECT_CHECKSUM;
    changes[*change_count].add = false;
    (*change_count)++;

    infilfs_native_checksum_group_cache_store(
        pending->sb, owner_id, target.object_id, stored_block, group_start);
    if (memcmp(tail_id, target.object_id, 16) == 0)
        tail_block = stored_block;
    memcpy(final_tail_id, tail_id, 16);
    *final_tail_block = tail_block;
    *final_tail_start = tail_start;
    ret = 0;
out:
    kfree(object);
    kfree(tail_object);
    return ret;
}

static int infilfs_native_checksum_append_tail(
    struct infilfs_native_pending *pending,
    struct infilfs_file_payload_disk *file, const u8 owner_id[16],
    u64 touched_start, u64 touched_count,
    const struct infilfs_data_checksum_disk *digests,
    struct infilfs_native_index_change *changes, u32 change_capacity,
    u32 *change_count, u8 final_tail_id[16], u64 *final_tail_block,
    u64 *final_tail_start)
{
    const u64 per = INFILFS_NATIVE_CHECKSUMS_PER_OBJECT;
    struct infilfs_native_checksum_node *new_nodes = NULL;
    struct infilfs_native_writer_tail cached_tail;
    struct infilfs_native_checksum_payload_disk *tail_payload = NULL;
    struct infilfs_data_checksum_disk *tail_values;
    u8 *tail_object = NULL;
    u64 touched_end;
    u64 tail_block = 0;
    u64 tail_start = 0;
    u32 tail_count = 0;
    u32 fill = 0;
    u32 new_count = 0;
    u32 i;
    int ret = 0;

    if (!touched_count || touched_start > U64_MAX - touched_count)
        return -EINVAL;
    touched_end = touched_start + touched_count;

    /*
     * Empty files are the simplest append case: create a short forward chain
     * containing only the checksum groups covered by this write.
     */
    if (!memchr_inv(file->checksum_head_id, 0,
                    sizeof(file->checksum_head_id))) {
        u64 group;

        if (touched_start != 0)
            return -EOPNOTSUPP;
        new_count = (u32)DIV_ROUND_UP_ULL(touched_count, per);
        if (!new_count || *change_count > change_capacity ||
            new_count > change_capacity - *change_count)
            return -ENOSPC;
        new_nodes = kvmalloc_array(new_count, sizeof(*new_nodes), GFP_NOFS);
        tail_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!new_nodes || !tail_object) {
            ret = -ENOMEM;
            goto out;
        }
        memset(new_nodes, 0, (size_t)new_count * sizeof(*new_nodes));
        group = 0;
        for (i = 0; i < new_count; ++i) {
            ret = infilfs_native_random_id(new_nodes[i].object_id);
            if (ret)
                goto out;
            new_nodes[i].start_logical = group;
            if (group > U64_MAX - per && i + 1u < new_count) {
                ret = -EOVERFLOW;
                goto out;
            }
            group += per;
        }
        for (i = 0; i < new_count; ++i) {
            const u8 *next_id = i + 1u < new_count ?
                new_nodes[i + 1u].object_id : NULL;
            u64 stored_block;

            ret = infilfs_native_init_checksum_object(
                pending, owner_id, new_nodes[i].object_id, next_id,
                new_nodes[i].start_logical, digests, touched_start,
                touched_count, tail_object);
            if (ret)
                goto out;
            ret = infilfs_rw_tx_alloc(&pending->tx, 1, &stored_block);
            if (ret)
                goto out;
            ret = infilfs_native_stage_block(
                pending->sb, stored_block, tail_object);
            if (ret)
                goto out;

            memcpy(changes[*change_count].object_id,
                   new_nodes[i].object_id, 16);
            changes[*change_count].object_block = stored_block;
            changes[*change_count].object_type = INFILFS_OBJECT_CHECKSUM;
            changes[*change_count].add = true;
            (*change_count)++;
            new_nodes[i].object_block = stored_block;
            infilfs_native_checksum_group_cache_store(
                pending->sb, owner_id, new_nodes[i].object_id,
                stored_block, new_nodes[i].start_logical);
        }
        memcpy(file->checksum_head_id, new_nodes[0].object_id, 16);
        memcpy(final_tail_id, new_nodes[new_count - 1u].object_id, 16);
        *final_tail_block = new_nodes[new_count - 1u].object_block;
        *final_tail_start = new_nodes[new_count - 1u].start_logical;
        ret = 0;
        goto out;
    }

    tail_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!tail_object)
        return -ENOMEM;

    /*
     * The writer-only tail cache is authoritative while write_lock is held:
     * readers never publish into it, namespace mutations invalidate it, and
     * rollback clears it.  A cache miss pays the historical walk once, then
     * all subsequent sequential appends stay at the tail.
     */
    if (infilfs_native_writer_tail_lookup(
            pending, owner_id, &cached_tail)) {
        ret = infilfs_native_checksum_decode(
            pending->sb, owner_id, cached_tail.object_id,
            cached_tail.object_block, tail_object, &tail_payload);
        if (!ret &&
            le64_to_cpu(tail_payload->start_logical_block) ==
                cached_tail.start_logical &&
            !memchr_inv(tail_payload->next_object_id, 0,
                        sizeof(tail_payload->next_object_id))) {
            memcpy(final_tail_id, cached_tail.object_id, 16);
            tail_block = cached_tail.object_block;
            tail_start = cached_tail.start_logical;
        } else {
            infilfs_native_writer_tail_invalidate(pending);
            tail_payload = NULL;
        }
    }

    if (!tail_payload) {
        ret = infilfs_native_checksum_tail(
            pending->sb, owner_id, file->checksum_head_id,
            final_tail_id, &tail_block, &tail_start, tail_object);
        if (ret)
            goto out;
        ret = infilfs_native_checksum_decode(
            pending->sb, owner_id, final_tail_id, tail_block,
            tail_object, &tail_payload);
        if (ret)
            goto out;
        infilfs_native_writer_tail_store(
            pending, owner_id, final_tail_id, tail_block, tail_start);
    }

    tail_count = le32_to_cpu(tail_payload->checksum_count);
    if (!tail_count || tail_count > per ||
        touched_start != tail_start + tail_count ||
        touched_end <= touched_start) {
        ret = -EOPNOTSUPP;
        goto out;
    }

    fill = min_t(u64, touched_count, per - tail_count);
    if (touched_count > fill)
        new_count = (u32)DIV_ROUND_UP_ULL(touched_count - fill, per);
    if (*change_count > change_capacity ||
        1u + new_count > change_capacity - *change_count) {
        ret = -ENOSPC;
        goto out;
    }

    if (new_count) {
        u64 group = tail_start + per;

        new_nodes = kvmalloc_array(new_count, sizeof(*new_nodes), GFP_NOFS);
        if (!new_nodes) {
            ret = -ENOMEM;
            goto out;
        }
        memset(new_nodes, 0, (size_t)new_count * sizeof(*new_nodes));
        for (i = 0; i < new_count; ++i) {
            ret = infilfs_native_random_id(new_nodes[i].object_id);
            if (ret)
                goto out;
            new_nodes[i].start_logical = group;
            if (group > U64_MAX - per && i + 1u < new_count) {
                ret = -EOVERFLOW;
                goto out;
            }
            group += per;
        }
    }

    tail_values = (struct infilfs_data_checksum_disk *)(tail_payload + 1);
    if (fill) {
        memcpy(tail_values + tail_count, digests,
               (size_t)fill * sizeof(*tail_values));
        tail_count += fill;
        tail_payload->checksum_count = cpu_to_le32(tail_count);
    }
    memset(tail_payload->next_object_id, 0,
           sizeof(tail_payload->next_object_id));
    if (new_count)
        memcpy(tail_payload->next_object_id, new_nodes[0].object_id, 16);
    ((struct infilfs_object_header_disk *)tail_object)->payload_size =
        cpu_to_le32(sizeof(*tail_payload) +
                    (size_t)tail_count * sizeof(*tail_values));
    ((struct infilfs_object_header_disk *)tail_object)->generation =
        cpu_to_le64(pending->tx.generation);
    ret = infilfs_rw_finalize_object(tail_object);
    if (ret)
        goto out;

    {
        u64 stored_block;

        ret = infilfs_native_store_private_or_cow(
            pending, tail_block, tail_object, &stored_block);
        if (ret)
            goto out;
        memcpy(changes[*change_count].object_id, final_tail_id, 16);
        changes[*change_count].object_block = stored_block;
        changes[*change_count].object_type = INFILFS_OBJECT_CHECKSUM;
        changes[*change_count].add = false;
        (*change_count)++;
        tail_block = stored_block;
        infilfs_native_checksum_group_cache_store(
            pending->sb, owner_id, final_tail_id, stored_block, tail_start);
    }

    for (i = 0; i < new_count; ++i) {
        const u8 *next_id = i + 1u < new_count ?
            new_nodes[i + 1u].object_id : NULL;
        u64 stored_block;

        ret = infilfs_native_init_checksum_object(
            pending, owner_id, new_nodes[i].object_id, next_id,
            new_nodes[i].start_logical, digests, touched_start,
            touched_count, tail_object);
        if (ret)
            goto out;
        ret = infilfs_rw_tx_alloc(&pending->tx, 1, &stored_block);
        if (ret)
            goto out;
        ret = infilfs_native_stage_block(
            pending->sb, stored_block, tail_object);
        if (ret)
            goto out;

        memcpy(changes[*change_count].object_id,
               new_nodes[i].object_id, 16);
        changes[*change_count].object_block = stored_block;
        changes[*change_count].object_type = INFILFS_OBJECT_CHECKSUM;
        changes[*change_count].add = true;
        (*change_count)++;
        new_nodes[i].object_block = stored_block;
        infilfs_native_checksum_group_cache_store(
            pending->sb, owner_id, new_nodes[i].object_id,
            stored_block, new_nodes[i].start_logical);
    }

    if (new_count) {
        memcpy(final_tail_id, new_nodes[new_count - 1u].object_id, 16);
        *final_tail_block = new_nodes[new_count - 1u].object_block;
        *final_tail_start = new_nodes[new_count - 1u].start_logical;
    } else {
        *final_tail_block = tail_block;
        *final_tail_start = tail_start;
    }
    ret = 0;
out:
    kvfree(new_nodes);
    kfree(tail_object);
    return ret;
}

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

    if (first_group == last_group) {
        ret = infilfs_native_checksum_update_existing_group(
            pending, file, owner_id, touched_start, touched_count, digests,
            changes, change_capacity, change_count, final_tail_id,
            final_tail_block, final_tail_start);
        if (!ret)
            return 0;
        if (ret != -ENOENT)
            return ret;
    }

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
        infilfs_native_checksum_group_cache_store(
            pending->sb, owner_id, nodes[i].object_id,
            stored_block, group_start);
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

enum infilfs_native_tree_mutation {
    INFILFS_NATIVE_TREE_REPOINT,
    INFILFS_NATIVE_TREE_ADD,
    INFILFS_NATIVE_TREE_REMOVE
};

static int infilfs_native_tree_build_subtree(
    struct infilfs_native_pending *pending, const u8 owner_id[16],
    const struct infilfs_index_entry_disk *entries, u32 count,
    unsigned int depth, u64 *root_out)
{
    u8 *block;
    int ret;

    if (!count || !root_out)
        return -EINVAL;
    block = kzalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!block)
        return -ENOMEM;

    if (count <= INFILFS_INDEX_ENTRIES_PER_PAGE) {
        struct infilfs_metadata_page_disk *page;
        u64 leaf;

        infilfs_rw_init_page(
            block, infilfs_index_page_magic, owner_id,
            pending->tx.generation);
        page = (struct infilfs_metadata_page_disk *)block;
        page->entry_count = cpu_to_le32(count);
        page->bytes_used = cpu_to_le32(
            count * sizeof(struct infilfs_index_entry_disk));
        memcpy(page + 1, entries, (size_t)count * sizeof(*entries));
        ret = infilfs_rw_finalize_page(block);
        if (!ret)
            ret = infilfs_rw_tx_alloc(&pending->tx, 1, &leaf);
        if (!ret)
            ret = infilfs_native_stage_block(pending->sb, leaf, block);
        if (!ret)
            *root_out = leaf;
        kfree(block);
        return ret;
    }
    if (depth >= 16u) {
        kfree(block);
        return -EFSCORRUPTED;
    }

    {
        struct infilfs_metadata_page_disk *page;
        __le64 *children;
        struct infilfs_index_entry_disk *group;
        u32 child_count = 0;
        u32 slot;
        u64 branch;

        infilfs_rw_init_page(
            block, infilfs_index_branch_page_magic, owner_id,
            pending->tx.generation);
        page = (struct infilfs_metadata_page_disk *)block;
        children = (__le64 *)(page + 1);
        group = kvmalloc_array(count, sizeof(*group), GFP_NOFS);
        if (!group) {
            kfree(block);
            return -ENOMEM;
        }

        ret = 0;
        for (slot = 0; slot < INFILFS_INDEX_TREE_FANOUT; ++slot) {
            u32 i;
            u32 n = 0;
            u64 child = 0;

            for (i = 0; i < count; ++i)
                if (entries[i].object_id[depth] == (u8)slot)
                    group[n++] = entries[i];
            if (!n)
                continue;
            ret = infilfs_native_tree_build_subtree(
                pending, owner_id, group, n, depth + 1u, &child);
            if (ret)
                break;
            children[slot] = cpu_to_le64(child);
            child_count++;
        }
        kvfree(group);
        if (ret) {
            kfree(block);
            return ret;
        }

        page->entry_count = cpu_to_le32(child_count);
        page->bytes_used = cpu_to_le32(INFILFS_INDEX_TREE_BRANCH_BYTES);
        ret = infilfs_rw_finalize_page(block);
        if (!ret)
            ret = infilfs_rw_tx_alloc(&pending->tx, 1, &branch);
        if (!ret)
            ret = infilfs_native_stage_block(pending->sb, branch, block);
        if (!ret)
            *root_out = branch;
    }
    kfree(block);
    return ret;
}

static int infilfs_native_tree_mutate_node(
    struct infilfs_native_pending *pending, const u8 owner_id[16], u64 node,
    const struct infilfs_native_index_change *change, unsigned int depth,
    enum infilfs_native_tree_mutation mutation, u64 *replacement_out)
{
    u8 *block;
    int ret;

    if (!node || depth > 16u)
        return -EFSCORRUPTED;
    block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!block)
        return -ENOMEM;
    ret = infilfs_read_allocated_block(pending->sb, node, block);
    if (ret)
        goto out;

    if (memcmp(block, infilfs_index_page_magic, 8) == 0) {
        struct infilfs_metadata_page_disk *page;
        struct infilfs_index_entry_disk *entries;
        u32 count;
        u32 found;
        u32 i;

        if (!infilfs_metadata_page_valid(
                pending->sb, block, infilfs_index_page_magic, owner_id)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        page = (struct infilfs_metadata_page_disk *)block;
        count = le32_to_cpu(page->entry_count);
        if (count > INFILFS_INDEX_ENTRIES_PER_PAGE ||
            le32_to_cpu(page->bytes_used) !=
                count * sizeof(struct infilfs_index_entry_disk)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        entries = (struct infilfs_index_entry_disk *)(page + 1);
        found = count;
        for (i = 0; i < count; ++i)
            if (memcmp(entries[i].object_id, change->object_id, 16) == 0) {
                found = i;
                break;
            }

        if (mutation == INFILFS_NATIVE_TREE_ADD) {
            if (found != count) {
                ret = -EEXIST;
                goto out;
            }
            if (count < INFILFS_INDEX_ENTRIES_PER_PAGE) {
                memset(&entries[count], 0, sizeof(entries[count]));
                memcpy(entries[count].object_id, change->object_id, 16);
                entries[count].object_block =
                    cpu_to_le64(change->object_block);
                entries[count].object_type =
                    cpu_to_le16(change->object_type);
                page->entry_count = cpu_to_le32(count + 1u);
                page->bytes_used = cpu_to_le32(
                    (count + 1u) * sizeof(*entries));
                page->generation = cpu_to_le64(pending->tx.generation);
                ret = infilfs_rw_finalize_page(block);
                if (!ret)
                    ret = infilfs_native_store_private_or_cow(
                        pending, node, block, replacement_out);
                goto out;
            }

            {
                struct infilfs_index_entry_disk *grown;

                grown = kvmalloc_array(
                    (size_t)count + 1u, sizeof(*grown), GFP_NOFS);
                if (!grown) {
                    ret = -ENOMEM;
                    goto out;
                }
                memcpy(grown, entries, (size_t)count * sizeof(*grown));
                memset(&grown[count], 0, sizeof(grown[count]));
                memcpy(grown[count].object_id, change->object_id, 16);
                grown[count].object_block =
                    cpu_to_le64(change->object_block);
                grown[count].object_type =
                    cpu_to_le16(change->object_type);
                ret = infilfs_native_tree_build_subtree(
                    pending, owner_id, grown, count + 1u, depth,
                    replacement_out);
                kvfree(grown);
                if (!ret)
                    ret = infilfs_rw_tx_defer_free(
                        &pending->tx, node, 1);
                goto out;
            }
        }

        if (found == count) {
            ret = -ENOENT;
            goto out;
        }
        if (mutation == INFILFS_NATIVE_TREE_REPOINT) {
            entries[found].object_block = cpu_to_le64(change->object_block);
            entries[found].object_type = cpu_to_le16(change->object_type);
            page->generation = cpu_to_le64(pending->tx.generation);
            ret = infilfs_rw_finalize_page(block);
            if (!ret)
                ret = infilfs_native_store_private_or_cow(
                    pending, node, block, replacement_out);
            goto out;
        }

        if (count == 1u) {
            ret = infilfs_rw_tx_defer_free(&pending->tx, node, 1);
            if (!ret)
                *replacement_out = 0;
            goto out;
        }
        if (found + 1u < count)
            memmove(&entries[found], &entries[found + 1u],
                    (size_t)(count - found - 1u) * sizeof(*entries));
        memset(&entries[count - 1u], 0, sizeof(*entries));
        page->entry_count = cpu_to_le32(count - 1u);
        page->bytes_used = cpu_to_le32(
            (count - 1u) * sizeof(*entries));
        page->generation = cpu_to_le64(pending->tx.generation);
        ret = infilfs_rw_finalize_page(block);
        if (!ret)
            ret = infilfs_native_store_private_or_cow(
                pending, node, block, replacement_out);
        goto out;
    }

    if (depth >= 16u) {
        ret = -EFSCORRUPTED;
        goto out;
    }

    {
        const __le64 *read_children;
        __le64 *children;
        struct infilfs_metadata_page_disk *page;
        u32 child_count;
        u32 slot = change->object_id[depth];
        u64 child;
        u64 replacement = 0;

        if (!infilfs_index_tree_branch_valid(
                pending->sb, block, owner_id,
                &read_children, &child_count)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        children = (__le64 *)(page = (struct infilfs_metadata_page_disk *)block,
                              page + 1);
        child = le64_to_cpu(read_children[slot]);

        if (!child) {
            struct infilfs_index_entry_disk entry;

            if (mutation != INFILFS_NATIVE_TREE_ADD) {
                ret = -ENOENT;
                goto out;
            }
            memset(&entry, 0, sizeof(entry));
            memcpy(entry.object_id, change->object_id, 16);
            entry.object_block = cpu_to_le64(change->object_block);
            entry.object_type = cpu_to_le16(change->object_type);
            ret = infilfs_native_tree_build_subtree(
                pending, owner_id, &entry, 1u, depth + 1u,
                &replacement);
            if (ret)
                goto out;
            child_count++;
        } else {
            ret = infilfs_native_tree_mutate_node(
                pending, owner_id, child, change, depth + 1u,
                mutation, &replacement);
            if (ret)
                goto out;
            if (!replacement)
                child_count--;
        }

        if (!child_count) {
            ret = infilfs_rw_tx_defer_free(&pending->tx, node, 1);
            if (!ret)
                *replacement_out = 0;
            goto out;
        }
        children[slot] = cpu_to_le64(replacement);
        page->entry_count = cpu_to_le32(child_count);
        page->generation = cpu_to_le64(pending->tx.generation);
        ret = infilfs_rw_finalize_page(block);
        if (!ret)
            ret = infilfs_native_store_private_or_cow(
                pending, node, block, replacement_out);
    }
out:
    kfree(block);
    return ret;
}

static int infilfs_native_index_update_tree(
    struct infilfs_native_pending *pending,
    struct infilfs_native_index_change *changes, u32 change_count,
    u8 head[INFILFS_DISK_BLOCK_SIZE])
{
    struct infilfs_sb_info *sbi = INFILFS_SB(pending->sb);
    struct infilfs_object_header_disk *header =
        (struct infilfs_object_header_disk *)head;
    struct infilfs_index_payload_disk *payload =
        (struct infilfs_index_payload_disk *)(header + 1);
    __le64 *root_ptr = (__le64 *)(payload + 1);
    u64 old_head_block = le64_to_cpu(sbi->disk.object_index_block);
    u64 root;
    u64 new_head_block;
    u32 total_count;
    u32 c;
    int ret = 0;

    if (le32_to_cpu(payload->reserved) != 0 ||
        le32_to_cpu(header->payload_size) !=
            sizeof(*payload) + sizeof(*root_ptr))
        return -EFSCORRUPTED;
    total_count = le32_to_cpu(payload->entry_count);
    root = le64_to_cpu(*root_ptr);
    if (!total_count || !root)
        return -EFSCORRUPTED;

    infilfs_native_index_locator_invalidate(pending);
    for (c = 0; c < change_count; ++c) {
        enum infilfs_native_tree_mutation mutation;
        u64 replacement = 0;

        if (changes[c].add && changes[c].remove)
            return -EINVAL;
        mutation = changes[c].add ? INFILFS_NATIVE_TREE_ADD :
            (changes[c].remove ? INFILFS_NATIVE_TREE_REMOVE :
                                 INFILFS_NATIVE_TREE_REPOINT);
        ret = infilfs_native_tree_mutate_node(
            pending, header->object_id, root, &changes[c], 0u,
            mutation, &replacement);
        if (ret)
            return ret;
        if (!replacement)
            return -EFSCORRUPTED;
        root = replacement;
        if (mutation == INFILFS_NATIVE_TREE_ADD) {
            if (total_count == U32_MAX)
                return -EOVERFLOW;
            total_count++;
        } else if (mutation == INFILFS_NATIVE_TREE_REMOVE) {
            if (total_count <= 1u)
                return -EFSCORRUPTED;
            total_count--;
        }
        changes[c].found = true;
    }

    payload->entry_count = cpu_to_le32(total_count);
    payload->reserved = 0;
    *root_ptr = cpu_to_le64(root);
    header->generation = cpu_to_le64(pending->tx.generation);
    header->payload_size = cpu_to_le32(sizeof(*payload) + sizeof(*root_ptr));
    ret = infilfs_rw_finalize_object(head);
    if (ret)
        return ret;
    ret = infilfs_native_store_private_or_cow(
        pending, old_head_block, head, &new_head_block);
    if (ret)
        return ret;
    pending->tx.next_sb.object_index_block = cpu_to_le64(new_head_block);
    sbi->disk = pending->tx.next_sb;
    return 0;
}

static int infilfs_native_index_update_classic(
    struct infilfs_native_pending *pending,
    struct infilfs_native_index_change *changes, u32 change_count)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(pending->sb);
    u32 i;
    int ret = 0;

    infilfs_native_index_locator_invalidate(pending);

    for (i = 0; i < change_count; ++i) {
        if (changes[i].remove)
            return -EOPNOTSUPP;
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
    struct infilfs_native_index_locator *located = NULL;
    u32 page_count = le32_to_cpu(payload->reserved);
    u32 total_count = le32_to_cpu(payload->entry_count);
    u32 c;
    u8 *page_block = NULL;
    u64 old_head_block = le64_to_cpu(sbi->disk.object_index_block);
    u64 new_head_block;
    bool rebuilt = false;
    int ret = 0;

    if (page_count > INFILFS_INDEX_PAGE_POINTERS)
        return -EFSCORRUPTED;
    for (c = 0; c < change_count; ++c)
        if (changes[c].remove)
            return -EOPNOTSUPP;
    page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    located = kcalloc(change_count ? change_count : 1u,
                      sizeof(*located), GFP_NOFS);
    if (!page_block || !located) {
        ret = -ENOMEM;
        goto out;
    }

locate_again:
    if (!pending->index_locator_valid) {
        ret = infilfs_native_index_locator_build(pending, head);
        if (ret)
            goto out;
    }
    ret = infilfs_native_index_locator_ensure(
        pending, total_count + change_count + 32u);
    if (ret)
        goto out;

    memset(located, 0, (size_t)(change_count ? change_count : 1u) *
                        sizeof(*located));
    for (c = 0; c < change_count; ++c) {
        u32 d;

        changes[c].found = false;
        if (changes[c].add) {
            if (infilfs_native_index_locator_lookup(
                    pending, changes[c].object_id, NULL, NULL)) {
                ret = -EEXIST;
                goto out;
            }
            for (d = 0; d < c; ++d)
                if (changes[d].add &&
                    memcmp(changes[d].object_id,
                           changes[c].object_id, 16) == 0) {
                    ret = -EEXIST;
                    goto out;
                }
            continue;
        }

        if (!infilfs_native_index_locator_lookup(
                pending, changes[c].object_id,
                &located[c].page_index, &located[c].entry_index)) {
            if (!rebuilt) {
                infilfs_native_index_locator_invalidate(pending);
                rebuilt = true;
                goto locate_again;
            }
            ret = -ENOENT;
            goto out;
        }
        located[c].valid = true;
    }

    /*
     * Validate all cached positions before mutating any page.  A full index
     * rebuild (for example after a remove) invalidates the locator cache, but
     * this check also makes stale/corrupt locator state fail closed.
     */
    for (c = 0; c < change_count; ++c) {
        struct infilfs_metadata_page_disk *page;
        struct infilfs_index_entry_disk *entries;
        u32 n;
        u32 p;

        if (changes[c].add)
            continue;
        p = located[c].page_index;
        if (p >= page_count) {
            ret = -ESTALE;
            goto stale_cache;
        }
        ret = infilfs_read_allocated_block(
            pending->sb, le64_to_cpu(pages[p]), page_block);
        if (ret)
            goto out;
        if (!infilfs_metadata_page_valid(
                pending->sb, page_block, infilfs_index_page_magic,
                header->object_id)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        page = (struct infilfs_metadata_page_disk *)page_block;
        n = le32_to_cpu(page->entry_count);
        if (n > INFILFS_INDEX_ENTRIES_PER_PAGE ||
            le32_to_cpu(page->bytes_used) !=
                n * sizeof(struct infilfs_index_entry_disk) ||
            located[c].entry_index >= n) {
            ret = -ESTALE;
            goto stale_cache;
        }
        entries = (struct infilfs_index_entry_disk *)(page + 1);
        if (memcmp(entries[located[c].entry_index].object_id,
                   changes[c].object_id, 16) != 0) {
            ret = -ESTALE;
            goto stale_cache;
        }
    }

    /*
     * Repoint only the index pages that actually contain changed objects.
     * Page and slot numbers stay stable under CoW, so the locator remains
     * valid after the physical metadata page itself moves.
     */
    for (c = 0; c < change_count; ++c) {
        struct infilfs_metadata_page_disk *page;
        struct infilfs_index_entry_disk *entries;
        u32 p;
        u32 d;
        u64 page_no;
        u64 replacement;
        bool dirty = false;

        if (changes[c].add || changes[c].found)
            continue;
        p = located[c].page_index;
        page_no = le64_to_cpu(pages[p]);
        ret = infilfs_read_allocated_block(pending->sb, page_no, page_block);
        if (ret)
            goto out;
        if (!infilfs_metadata_page_valid(
                pending->sb, page_block, infilfs_index_page_magic,
                header->object_id)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        page = (struct infilfs_metadata_page_disk *)page_block;
        entries = (struct infilfs_index_entry_disk *)(page + 1);

        for (d = c; d < change_count; ++d) {
            u32 entry_index;

            if (changes[d].add || changes[d].found ||
                located[d].page_index != p)
                continue;
            entry_index = located[d].entry_index;
            if (entry_index >= le32_to_cpu(page->entry_count) ||
                memcmp(entries[entry_index].object_id,
                       changes[d].object_id, 16) != 0) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            entries[entry_index].object_block =
                cpu_to_le64(changes[d].object_block);
            entries[entry_index].object_type =
                cpu_to_le16(changes[d].object_type);
            changes[d].found = true;
            dirty = true;
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

    /*
     * New IDs are append-only in the paged index.  The complete in-memory
     * locator proves they are absent without rereading every prior page.
     */
    c = 0;
    while (c < change_count) {
        struct infilfs_metadata_page_disk *page;
        struct infilfs_index_entry_disk *entries;
        u32 n = 0;
        u32 page_index;
        u64 page_no = 0;
        u64 replacement;

        while (c < change_count && !changes[c].add)
            ++c;
        if (c >= change_count)
            break;

        if (page_count) {
            page_index = page_count - 1u;
            page_no = le64_to_cpu(pages[page_index]);
            ret = infilfs_read_allocated_block(pending->sb, page_no, page_block);
            if (ret)
                goto out;
            if (!infilfs_metadata_page_valid(
                    pending->sb, page_block, infilfs_index_page_magic,
                    header->object_id)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            page = (struct infilfs_metadata_page_disk *)page_block;
            n = le32_to_cpu(page->entry_count);
            if (n > INFILFS_INDEX_ENTRIES_PER_PAGE ||
                le32_to_cpu(page->bytes_used) !=
                    n * sizeof(struct infilfs_index_entry_disk)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
        } else {
            page_index = 0;
            infilfs_rw_init_page(page_block, infilfs_index_page_magic,
                                 header->object_id, pending->tx.generation);
            page = (struct infilfs_metadata_page_disk *)page_block;
        }

        if (n == INFILFS_INDEX_ENTRIES_PER_PAGE) {
            if (page_count >= INFILFS_INDEX_PAGE_POINTERS) {
                ret = -ENOSPC;
                goto out;
            }
            page_index = page_count;
            infilfs_rw_init_page(page_block, infilfs_index_page_magic,
                                 header->object_id, pending->tx.generation);
            page = (struct infilfs_metadata_page_disk *)page_block;
            n = 0;
            page_no = 0;
        }

        entries = (struct infilfs_index_entry_disk *)(page + 1);
        while (c < change_count && n < INFILFS_INDEX_ENTRIES_PER_PAGE) {
            u32 entry_index;

            if (!changes[c].add) {
                ++c;
                continue;
            }
            entry_index = n;
            memset(&entries[n], 0, sizeof(entries[n]));
            memcpy(entries[n].object_id, changes[c].object_id, 16);
            entries[n].object_block = cpu_to_le64(changes[c].object_block);
            entries[n].object_type = cpu_to_le16(changes[c].object_type);
            ret = infilfs_native_index_locator_insert(
                pending, changes[c].object_id, page_index, entry_index);
            if (ret)
                goto out;
            changes[c].found = true;
            n++;
            total_count++;
            ++c;
        }

        page->entry_count = cpu_to_le32(n);
        page->bytes_used = cpu_to_le32(
            n * sizeof(struct infilfs_index_entry_disk));
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
            pages[page_index] = cpu_to_le64(replacement);
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
    goto out;

stale_cache:
    if (!rebuilt) {
        infilfs_native_index_locator_invalidate(pending);
        rebuilt = true;
        goto locate_again;
    }
    if (ret == -ESTALE)
        ret = -EFSCORRUPTED;
out:
    if (ret)
        infilfs_native_index_locator_invalidate(pending);
    kfree(located);
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
    if (le16_to_cpu(header->object_version) == INFILFS_OBJECT_VERSION_TREE)
        ret = infilfs_native_index_update_tree(
            pending, changes, change_count, head);
    else if (le16_to_cpu(header->object_version) ==
             INFILFS_OBJECT_VERSION_PAGED)
        ret = infilfs_native_index_update_paged(
            pending, changes, change_count, head);
    else
        ret = infilfs_native_index_update_classic(
            pending, changes, change_count);
out:
    kfree(head);
    return ret;
}

static int infilfs_native_store_extent_page(
    struct infilfs_native_pending *pending, u64 old_block,
    const u8 page[INFILFS_DISK_BLOCK_SIZE], u64 *new_block_out)
{
    u64 new_block;
    int ret;

    /*
     * Reuse transaction-private pages when the per-operation undo journal has
     * room.  If it is already full, replace the private page instead of
     * failing the write with -E2BIG; the old private page is then reclaimed at
     * transaction publication.
     */
    if (old_block)
        return infilfs_native_store_private_or_cow(
            pending, old_block, page, new_block_out);

    ret = infilfs_rw_tx_alloc(&pending->tx, 1, &new_block);
    if (ret)
        return ret;
    ret = infilfs_native_stage_block(pending->sb, new_block, page);
    if (ret)
        return ret;
    *new_block_out = new_block;
    return 0;
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
    const struct infilfs_file_payload_disk *old_file =
        (const struct infilfs_file_payload_disk *)(old_header + 1);
    const __le64 *old_pointers = NULL;
    u32 old_page_count = 0;
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

    if (le16_to_cpu(old_header->object_version) ==
        INFILFS_OBJECT_VERSION_PAGED) {
        const struct infilfs_extent_head_disk *old_head =
            (const struct infilfs_extent_head_disk *)(old_file + 1);
        size_t expected;

        old_page_count = le32_to_cpu(old_head->page_count);
        if (!old_page_count ||
            old_page_count > INFILFS_EXTENT_PAGE_POINTERS ||
            le32_to_cpu(old_head->reserved) != 0)
            return -EFSCORRUPTED;
        expected = sizeof(*old_file) + sizeof(*old_head) +
            (size_t)old_page_count * sizeof(__le64);
        if (expected != le32_to_cpu(old_header->payload_size))
            return -EFSCORRUPTED;
        old_pointers = (const __le64 *)(old_head + 1);
    }

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
        u32 p;

        header->object_version = cpu_to_le16(INFILFS_OBJECT_VERSION_CLASSIC);
        header->payload_size = cpu_to_le32(sizeof(*file) +
            (size_t)extent_count * sizeof(*extents));
        if (extent_count)
            memcpy(file + 1, extents, (size_t)extent_count * sizeof(*extents));

        /*
         * collect_extents() is read-only with respect to metadata ownership.
         * When a paged file collapses back to the classic representation,
         * reclaim its former extent pages here, at the point where they
         * actually become unreachable.
         */
        for (p = 0; p < old_page_count; ++p) {
            ret = infilfs_rw_tx_defer_free(
                &pending->tx, le64_to_cpu(old_pointers[p]), 1);
            if (ret)
                return ret;
        }
    } else {
        struct infilfs_extent_head_disk *head =
            (struct infilfs_extent_head_disk *)(file + 1);
        __le64 *pointers = (__le64 *)(head + 1);
        u32 pages = DIV_ROUND_UP(extent_count, INFILFS_EXTENTS_PER_PAGE);
        u32 copied = 0;
        u32 p;
        u8 *page_block;
        u8 *old_page_block;

        if (!(le64_to_cpu(pending->tx.next_sb.incompat_flags) &
              INFILFS_INCOMPAT_PAGED_EXTENTS) ||
            !pages || pages > INFILFS_EXTENT_PAGE_POINTERS)
            return -EFBIG;
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        old_page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!page_block || !old_page_block) {
            kfree(old_page_block);
            kfree(page_block);
            return -ENOMEM;
        }

        header->object_version = cpu_to_le16(INFILFS_OBJECT_VERSION_PAGED);
        head->page_count = cpu_to_le32(pages);
        head->reserved = 0;

        for (p = 0; p < pages; ++p) {
            struct infilfs_metadata_page_disk *page;
            u32 n = min_t(u32, INFILFS_EXTENTS_PER_PAGE,
                          extent_count - copied);
            u64 old_page_no = p < old_page_count ?
                le64_to_cpu(old_pointers[p]) : 0;
            u64 block;
            bool unchanged = false;

            infilfs_rw_init_page(page_block, infilfs_extent_page_magic,
                                 ii->object_id, pending->tx.generation);
            page = (struct infilfs_metadata_page_disk *)page_block;
            page->entry_count = cpu_to_le32(n);
            page->bytes_used = cpu_to_le32(n * sizeof(*extents));
            memcpy(page + 1, extents + copied, (size_t)n * sizeof(*extents));

            if (old_page_no) {
                struct infilfs_metadata_page_disk *old_page;
                u32 old_n;

                ret = infilfs_read_allocated_block(pending->sb, old_page_no,
                                         old_page_block);
                if (ret)
                    goto paged_out;
                if (!infilfs_metadata_page_valid(
                        pending->sb, old_page_block,
                        infilfs_extent_page_magic, ii->object_id)) {
                    ret = -EFSCORRUPTED;
                    goto paged_out;
                }
                old_page =
                    (struct infilfs_metadata_page_disk *)old_page_block;
                old_n = le32_to_cpu(old_page->entry_count);
                if (old_n == n &&
                    le32_to_cpu(old_page->bytes_used) ==
                        n * sizeof(*extents) &&
                    memcmp(old_page + 1, extents + copied,
                           (size_t)n * sizeof(*extents)) == 0)
                    unchanged = true;
            }

            if (unchanged) {
                pointers[p] = cpu_to_le64(old_page_no);
                copied += n;
                continue;
            }

            ret = infilfs_rw_finalize_page(page_block);
            if (ret)
                goto paged_out;
            ret = infilfs_native_store_extent_page(
                pending, old_page_no, page_block, &block);
            if (ret)
                goto paged_out;
            pointers[p] = cpu_to_le64(block);
            copied += n;
        }

        /*
         * A shrinking paged extent map may use fewer pages than the previous
         * file object.  Those metadata pages are no longer reachable from the
         * live object and must participate in the same deferred/snapshot-aware
         * reclamation as every other CoW metadata block.
         */
        for (p = pages; p < old_page_count; ++p) {
            ret = infilfs_rw_tx_defer_free(
                &pending->tx, le64_to_cpu(old_pointers[p]), 1);
            if (ret)
                goto paged_out;
        }

        header->payload_size = cpu_to_le32(sizeof(*file) + sizeof(*head) +
            (size_t)pages * sizeof(__le64));
        ret = 0;
paged_out:
        kfree(old_page_block);
        kfree(page_block);
        if (ret)
            return ret;
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


static int infilfs_native_build_extent_page(
    struct infilfs_native_pending *pending, const u8 owner_id[16],
    const struct infilfs_extent_disk *extents, u32 count,
    u8 page_block[INFILFS_DISK_BLOCK_SIZE])
{
    struct infilfs_metadata_page_disk *page;

    if (!count || count > INFILFS_EXTENTS_PER_PAGE)
        return -EINVAL;
    infilfs_rw_init_page(page_block, infilfs_extent_page_magic,
                         owner_id, pending->tx.generation);
    page = (struct infilfs_metadata_page_disk *)page_block;
    page->entry_count = cpu_to_le32(count);
    page->bytes_used = cpu_to_le32(count * sizeof(*extents));
    memcpy(page + 1, extents, (size_t)count * sizeof(*extents));
    return infilfs_rw_finalize_page(page_block);
}

/*
 * Fast path for the workload that exposed the mounted random-write scaling
 * defect: one aligned 4 KiB overwrite inside an existing paged file.
 *
 * The generic path remains authoritative for growth, partial-block writes,
 * inline files, sparse insertion and multi-block updates.  This path avoids
 * flattening every extent page merely to replace one logical block.  It
 * binary-searches the paged map, rewrites only the affected extent page
 * (splitting that page locally when necessary), updates the one checksum
 * group, and CoWs the file head.
 */
static int infilfs_native_try_single_paged_overwrite(
    struct infilfs_native_pending *pending, struct inode *inode,
    const u8 write_data[INFILFS_DISK_BLOCK_SIZE], u64 pos, size_t bytes,
    const u8 old_object[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_parallel_reservation *reservation,
    u64 *new_file_block_out, u64 *new_size_out,
    u64 *allocated_blocks_out, u8 final_tail_id[16],
    u64 *final_tail_block, u64 *final_tail_start)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    const struct infilfs_object_header_disk *old_header =
        (const struct infilfs_object_header_disk *)old_object;
    const struct infilfs_file_payload_disk *old_file =
        (const struct infilfs_file_payload_disk *)(old_header + 1);
    const struct infilfs_extent_head_disk *old_head;
    const __le64 *old_pointers;
    struct infilfs_native_index_change changes[4] = {0};
    struct infilfs_file_payload_disk working_file;
    struct infilfs_extent_disk *local = NULL;
    struct infilfs_extent_disk *entries;
    struct infilfs_metadata_page_disk *page;
    struct infilfs_object_header_disk *new_header;
    struct infilfs_file_payload_disk *new_file;
    struct infilfs_extent_head_disk *new_head;
    __le64 *new_pointers;
    struct infilfs_data_checksum_disk digest;
    u8 *page_block = NULL;
    u8 *new_object = NULL;
    u64 logical;
    u64 old_size;
    u64 old_page_block = 0;
    u64 replacement_block = 0;
    u64 first_page_block = 0;
    u64 second_page_block = 0;
    u64 old_physical = 0;
    u64 extent_start;
    u64 extent_end;
    u32 old_flags;
    u32 old_page_count;
    u32 old_extent_total;
    u32 page_index = 0;
    u32 extent_index = 0;
    u32 page_entries;
    u32 local_count = 0;
    u32 new_extent_total;
    u32 change_count = 0;
    u32 lo, hi;
    bool file_repoint = false;
    bool found_page = false;
    bool found_extent = false;
    int ret = -EOPNOTSUPP;

    if (bytes != INFILFS_DISK_BLOCK_SIZE ||
        (pos & (INFILFS_DISK_BLOCK_SIZE - 1u)) != 0 ||
        le16_to_cpu(old_header->object_version) != INFILFS_OBJECT_VERSION_PAGED)
        return -EOPNOTSUPP;

    old_size = le64_to_cpu(old_file->attributes.logical_size);
    if (pos > old_size || bytes > old_size - pos ||
        !le32_to_cpu(old_file->extent_count))
        return -EOPNOTSUPP;

    old_head = (const struct infilfs_extent_head_disk *)(old_file + 1);
    old_page_count = le32_to_cpu(old_head->page_count);
    old_extent_total = le32_to_cpu(old_file->extent_count);
    if (!old_page_count || old_page_count > INFILFS_EXTENT_PAGE_POINTERS ||
        le32_to_cpu(old_head->reserved) != 0 ||
        sizeof(*old_file) + sizeof(*old_head) +
            (size_t)old_page_count * sizeof(__le64) !=
                le32_to_cpu(old_header->payload_size))
        return -EFSCORRUPTED;
    old_pointers = (const __le64 *)(old_head + 1);
    logical = pos >> INFILFS_DISK_BLOCK_SHIFT;

    page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    new_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    local = kvmalloc_array(INFILFS_EXTENTS_PER_PAGE + 2u,
                           sizeof(*local), GFP_NOFS);
    if (!page_block || !new_object || !local) {
        ret = -ENOMEM;
        goto out;
    }

    /*
     * Every extent page covers one continuous logical interval.  Binary
     * search those intervals instead of walking all pages and materialising
     * the complete map.
     */
    lo = 0;
    hi = old_page_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        u64 block = le64_to_cpu(old_pointers[mid]);
        u64 first_logical;
        u64 last_logical;
        u32 n;

        ret = infilfs_read_allocated_block(inode->i_sb, block, page_block);
        if (ret)
            goto out;
        if (!infilfs_metadata_page_valid(inode->i_sb, page_block,
                                         infilfs_extent_page_magic,
                                         ii->object_id)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        page = (struct infilfs_metadata_page_disk *)page_block;
        n = le32_to_cpu(page->entry_count);
        if (!n || n > INFILFS_EXTENTS_PER_PAGE ||
            le32_to_cpu(page->bytes_used) != n * sizeof(*local)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        entries = (struct infilfs_extent_disk *)(page + 1);
        first_logical = le64_to_cpu(entries[0].logical_block);
        last_logical = le64_to_cpu(entries[n - 1u].logical_block) +
            le32_to_cpu(entries[n - 1u].block_count);
        if (last_logical <= first_logical) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        if (logical < first_logical) {
            hi = mid;
        } else if (logical >= last_logical) {
            lo = mid + 1u;
        } else {
            page_index = mid;
            old_page_block = block;
            page_entries = n;
            found_page = true;
            break;
        }
    }
    if (!found_page) {
        ret = -EFSCORRUPTED;
        goto out;
    }

    page = (struct infilfs_metadata_page_disk *)page_block;
    entries = (struct infilfs_extent_disk *)(page + 1);
    lo = 0;
    hi = page_entries;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        u64 start = le64_to_cpu(entries[mid].logical_block);
        u64 end = start + le32_to_cpu(entries[mid].block_count);

        if (end <= start) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        if (logical < start)
            hi = mid;
        else if (logical >= end)
            lo = mid + 1u;
        else {
            extent_index = mid;
            found_extent = true;
            break;
        }
    }
    if (!found_extent) {
        ret = -EFSCORRUPTED;
        goto out;
    }

    extent_start = le64_to_cpu(entries[extent_index].logical_block);
    extent_end = extent_start +
        le32_to_cpu(entries[extent_index].block_count);
    old_flags = le32_to_cpu(entries[extent_index].flags);
    if (old_flags != INFILFS_EXTENT_NORMAL &&
        old_flags != INFILFS_EXTENT_HOLE) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    if (old_flags == INFILFS_EXTENT_HOLE) {
        ret = -EOPNOTSUPP;
        goto out;
    }
    {
        u64 base = le64_to_cpu(entries[extent_index].physical_block);

        if (!base) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        old_physical = base + logical - extent_start;
    }

    ret = infilfs_native_alloc_data_exact_reserved(
        pending, 1,
        old_physical && old_physical + 1u < pending->tx.sbi->device_blocks ?
            old_physical + 1u : 1u,
        INFILFS_DATA_WORKLOAD_RANDOM, reservation, &replacement_block);
    if (ret)
        goto out;

    ret = infilfs_native_stage_block(inode->i_sb, replacement_block,
                                     write_data);
    if (ret)
        goto out;
    infilfs_native_block_digest(write_data, &digest);

    /*
     * Rebuild only the target page in canonical order.  At most two entries
     * are added by replacing one block in the middle of an existing extent.
     */
    for (lo = 0; lo < page_entries; ++lo) {
        u64 start = le64_to_cpu(entries[lo].logical_block);
        u64 physical = le64_to_cpu(entries[lo].physical_block);
        u32 count = le32_to_cpu(entries[lo].block_count);
        u32 flags = le32_to_cpu(entries[lo].flags);

        if (lo != extent_index) {
            ret = infilfs_native_extent_append(
                local, INFILFS_EXTENTS_PER_PAGE + 2u, &local_count,
                start, physical, count, flags);
            if (ret)
                goto out;
            continue;
        }

        if (logical > extent_start) {
            ret = infilfs_native_extent_append(
                local, INFILFS_EXTENTS_PER_PAGE + 2u, &local_count,
                extent_start,
                old_flags == INFILFS_EXTENT_NORMAL ?
                    le64_to_cpu(entries[lo].physical_block) : 0,
                (u32)(logical - extent_start), old_flags);
            if (ret)
                goto out;
        }
        ret = infilfs_native_extent_append(
            local, INFILFS_EXTENTS_PER_PAGE + 2u, &local_count,
            logical, replacement_block, 1, INFILFS_EXTENT_NORMAL);
        if (ret)
            goto out;
        if (logical + 1u < extent_end) {
            ret = infilfs_native_extent_append(
                local, INFILFS_EXTENTS_PER_PAGE + 2u, &local_count,
                logical + 1u,
                old_flags == INFILFS_EXTENT_NORMAL ?
                    le64_to_cpu(entries[lo].physical_block) +
                        (logical + 1u - extent_start) : 0,
                (u32)(extent_end - logical - 1u), old_flags);
            if (ret)
                goto out;
        }
    }

    if (old_extent_total < page_entries) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    new_extent_total = old_extent_total - page_entries + local_count;

    memcpy(&working_file, old_file, sizeof(working_file));
    ret = infilfs_native_checksum_update_existing_group(
        pending, &working_file, ii->object_id, logical, 1, &digest,
        changes, ARRAY_SIZE(changes), &change_count,
        final_tail_id, final_tail_block, final_tail_start);
    if (ret)
        goto out;

    ret = infilfs_ns_free_unshared_run(
        pending, ii->object_id, old_physical, 1);
    if (ret)
        goto out;

    memcpy(new_object, old_object, INFILFS_DISK_BLOCK_SIZE);
    new_header = (struct infilfs_object_header_disk *)new_object;
    new_file = (struct infilfs_file_payload_disk *)(new_header + 1);
    new_head = (struct infilfs_extent_head_disk *)(new_file + 1);
    new_pointers = (__le64 *)(new_head + 1);

    if (local_count <= INFILFS_EXTENTS_PER_PAGE) {
        ret = infilfs_native_build_extent_page(
            pending, ii->object_id, local, local_count, page_block);
        if (ret)
            goto out;
        ret = infilfs_native_store_extent_page(
            pending, old_page_block, page_block, &first_page_block);
        if (ret)
            goto out;
        new_pointers[page_index] = cpu_to_le64(first_page_block);
    } else {
        u32 first_count;
        u32 second_count;

        if (old_page_count >= INFILFS_EXTENT_PAGE_POINTERS) {
            ret = -EFBIG;
            goto out;
        }
        first_count = local_count / 2u;
        second_count = local_count - first_count;
        if (!first_count || first_count > INFILFS_EXTENTS_PER_PAGE ||
            !second_count || second_count > INFILFS_EXTENTS_PER_PAGE) {
            ret = -EFSCORRUPTED;
            goto out;
        }

        ret = infilfs_native_build_extent_page(
            pending, ii->object_id, local, first_count, page_block);
        if (ret)
            goto out;
        ret = infilfs_native_store_extent_page(
            pending, old_page_block, page_block, &first_page_block);
        if (ret)
            goto out;

        ret = infilfs_native_build_extent_page(
            pending, ii->object_id, local + first_count,
            second_count, page_block);
        if (ret)
            goto out;
        ret = infilfs_native_store_extent_page(
            pending, 0, page_block, &second_page_block);
        if (ret)
            goto out;

        memmove(&new_pointers[page_index + 2u],
                &new_pointers[page_index + 1u],
                (size_t)(old_page_count - page_index - 1u) *
                    sizeof(*new_pointers));
        new_pointers[page_index] = cpu_to_le64(first_page_block);
        new_pointers[page_index + 1u] = cpu_to_le64(second_page_block);
        old_page_count++;
        new_head->page_count = cpu_to_le32(old_page_count);
        new_header->payload_size = cpu_to_le32(
            sizeof(*new_file) + sizeof(*new_head) +
            (size_t)old_page_count * sizeof(*new_pointers));
    }

    memcpy(new_file->checksum_head_id, working_file.checksum_head_id, 16);
    new_file->extent_count = cpu_to_le32(new_extent_total);
    new_file->data_checksum_type = cpu_to_le32(INFILFS_CHECKSUM_SHA256);
    new_file->attributes.modification_time_ns = cpu_to_le64(ktime_get_real_ns());
    new_file->attributes.change_time_ns =
        new_file->attributes.modification_time_ns;
    new_header->generation = cpu_to_le64(pending->tx.generation);

    ret = infilfs_rw_finalize_object(new_object);
    if (ret)
        goto out;
    ret = infilfs_native_store_private_or_cow(
        pending, ii->object_block, new_object,
        new_file_block_out);
    if (ret)
        goto out;
    file_repoint = *new_file_block_out != ii->object_block;
    if (file_repoint) {
        if (change_count >= ARRAY_SIZE(changes)) {
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
    *new_size_out = old_size;
    *allocated_blocks_out =
        inode->i_blocks / (INFILFS_DISK_BLOCK_SIZE >> 9);
    ret = 0;
out:
    kvfree(local);
    kfree(new_object);
    kfree(page_block);
    return ret;
}

static int infilfs_native_append_chunk_locked(
    struct infilfs_native_pending *pending, struct inode *inode,
    struct iov_iter *from, u64 pos, size_t bytes,
    struct infilfs_parallel_reservation *reservation,
    size_t *consumed_out,
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
    enum infilfs_data_workload workload;
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
    ret = infilfs_read_object(inode->i_sb, ii->object_block,
                              INFILFS_OBJECT_FILE, ii->object_id, old_object);
    if (ret)
        goto out;
    old_header = (struct infilfs_object_header_disk *)old_object;
    old_file = (struct infilfs_file_payload_disk *)(old_header + 1);
    if (bytes == INFILFS_DISK_BLOCK_SIZE &&
        (pos & (INFILFS_DISK_BLOCK_SIZE - 1u)) == 0 &&
        le16_to_cpu(old_header->object_version) ==
            INFILFS_OBJECT_VERSION_PAGED &&
        pos <= le64_to_cpu(old_file->attributes.logical_size) &&
        bytes <= le64_to_cpu(old_file->attributes.logical_size) - pos) {
        u8 *fast_data = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        size_t fast_copied;

        if (!fast_data) {
            ret = -ENOMEM;
            goto out;
        }
        fast_copied = copy_from_iter(
            fast_data, INFILFS_DISK_BLOCK_SIZE, from);
        if (fast_copied != INFILFS_DISK_BLOCK_SIZE) {
            if (fast_copied)
                iov_iter_revert(from, fast_copied);
            kfree(fast_data);
            goto generic_extent_write;
        }
        ret = infilfs_native_try_single_paged_overwrite(
            pending, inode, fast_data, pos, bytes, old_object,
            reservation,
            new_file_block_out, new_size_out, allocated_blocks_out,
            final_tail_id, final_tail_block, final_tail_start);
        kfree(fast_data);
        if (!ret) {
            /*
             * The common exit publishes the local consumed count.  Record the
             * successful fast-path byte count there so the outer write loop
             * advances exactly once; writing consumed_out directly here would
             * be overwritten at the common exit and re-enter with an exhausted
             * iov_iter.
             */
            consumed = INFILFS_DISK_BLOCK_SIZE;
            goto out;
        }
        iov_iter_revert(from, INFILFS_DISK_BLOCK_SIZE);
        if (ret != -EOPNOTSUPP)
            goto out;
    }

generic_extent_write:
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

    workload = infilfs_native_classify_write(pos, bytes, old_size);
    preferred = READ_ONCE(ii->data_allocation_hint);
    if (preferred < 1u || preferred >= pending->tx.sbi->device_blocks)
        preferred = infilfs_parallel_object_preferred(
            pending->tx.sbi, ii->object_id);

    if (workload == INFILFS_DATA_WORKLOAD_RANDOM && old_extent_count) {
        u64 old_physical = 0;
        u32 old_flags = INFILFS_EXTENT_HOLE;

        if (infilfs_native_map_extent(
                old_extents, old_extent_count, start_block,
                &old_physical, &old_flags) == 0 &&
            old_flags == INFILFS_EXTENT_NORMAL)
            preferred = old_physical;
    } else if (old_extent_count) {
        struct infilfs_extent_disk *last = &old_extents[old_extent_count - 1u];

        if (le32_to_cpu(last->flags) == INFILFS_EXTENT_NORMAL)
            preferred = le64_to_cpu(last->physical_block) +
                le32_to_cpu(last->block_count);
    }

    ret = infilfs_native_alloc_data_exact_reserved(
        pending, touched_blocks, preferred, workload,
        reservation, &data_start);
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
            ret = infilfs_read_allocated_block(
                inode->i_sb, old_physical, data_block);
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
    if (pos == old_size &&
        (pos & (INFILFS_DISK_BLOCK_SIZE - 1u)) == 0 &&
        start_block == old_blocks) {
        ret = infilfs_native_checksum_append_tail(
            pending, &working_file, ii->object_id,
            start_block, touched_blocks, digests,
            changes, change_capacity, &change_count,
            final_tail_id, final_tail_block, final_tail_start);
        if (ret == -EOPNOTSUPP)
            ret = infilfs_native_checksum_append(
                pending, &working_file, ii->object_id,
                start_block, touched_blocks, digests,
                changes, change_capacity, &change_count,
                final_tail_id, final_tail_block, final_tail_start);
    } else {
        ret = infilfs_native_checksum_append(
            pending, &working_file, ii->object_id,
            start_block, touched_blocks, digests,
            changes, change_capacity, &change_count,
            final_tail_id, final_tail_block, final_tail_start);
    }
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
    struct infilfs_parallel_reservation reservation = {0};
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

    /*
     * Reserve the first bounded data chunk before entering write_lock.  Many
     * independent writers can search and reserve separate allocation arenas
     * concurrently; the serialized metadata transaction later consumes the
     * reservation atomically.  Failure simply falls back to the authoritative
     * transaction allocator, preserving near-full behaviour.
     */
    {
        size_t first_bytes = min_t(size_t, requested,
                                   INFILFS_NATIVE_WRITE_CHUNK);
        u64 first_end = (u64)pos + first_bytes;
        u64 first_block = (u64)pos >> INFILFS_DISK_BLOCK_SHIFT;
        u64 last_block = (first_end - 1u) >> INFILFS_DISK_BLOCK_SHIFT;
        u64 old_size = (u64)i_size_read(inode);
        enum infilfs_data_workload workload =
            infilfs_native_classify_write((u64)pos, first_bytes, old_size);
        u64 preferred = READ_ONCE(ii->data_allocation_hint);

        if (preferred < 1u || preferred >= sbi->device_blocks)
            preferred = infilfs_parallel_object_preferred(
                sbi, ii->object_id);

        /*
         * Keep lockless multi-writer reservations for streaming growth. Small
         * random/sparse CoW writes enter the scored transaction allocator so
         * they can best-fit into existing holes instead of consuming the head
         * of a large free run.
         */
        if (workload == INFILFS_DATA_WORKLOAD_SEQUENTIAL &&
            first_end >= (u64)pos &&
            infilfs_parallel_reserve_data(
                inode->i_sb, last_block - first_block + 1u, preferred,
                &reservation) == 0)
            WRITE_ONCE(ii->data_allocation_hint,
                reservation.start + reservation.count < sbi->device_blocks ?
                    reservation.start + reservation.count : 1u);
    }

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

        for (;;) {
            ret = infilfs_native_operation_snapshot(pending);
            if (ret)
                break;
            ret = infilfs_native_append_chunk_locked(
                pending, inode, from, (u64)pos + done, chunk,
                reservation.active ? &reservation : NULL, &consumed,
                &new_file_block, &new_size, &allocated_blocks, tail_id,
                &tail_block, &tail_start);
            if (!ret)
                break;

            {
                int rollback = infilfs_native_operation_rollback(pending);
                if (rollback) {
                    ret = rollback;
                    break;
                }
            }

            /*
             * Total free space can remain healthy while fragmentation leaves
             * no run large enough for the preferred write chunk. Retry the
             * same userspace write with progressively smaller contiguous
             * pieces. The one-block-boundary floor guarantees we eventually
             * ask only for one physical filesystem block before reporting a
             * genuine ENOSPC.
             */
            if (ret != -ENOSPC)
                break;
            {
                size_t offset =
                    ((size_t)((u64)pos + done)) &
                    (INFILFS_DISK_BLOCK_SIZE - 1u);
                size_t one_block = INFILFS_DISK_BLOCK_SIZE - offset;
                size_t next;

                if (chunk <= one_block)
                    break;
                next = chunk / 2u;
                if (next < one_block)
                    next = one_block;
                if (next >= chunk)
                    break;
                chunk = next;
                consumed = 0;
                new_file_block = 0;
                new_size = 0;
                allocated_blocks = 0;
                memset(tail_id, 0, sizeof(tail_id));
                tail_block = 0;
                tail_start = 0;
            }
        }
        if (ret)
            break;

        infilfs_native_operation_commit(pending);
        ii->object_block = new_file_block;
        i_size_write(inode, new_size);
        inode->i_blocks = allocated_blocks *
            (INFILFS_DISK_BLOCK_SIZE >> 9);
        pending->tx.next_sb.generation = cpu_to_le64(pending->tx.generation);
        sbi->disk = pending->tx.next_sb;
        infilfs_native_checksum_cache_store(
            inode->i_sb, ii->object_id, tail_id, tail_block, tail_start);
        infilfs_native_writer_tail_store(
            pending, ii->object_id, tail_id, tail_block, tail_start);
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
    infilfs_parallel_release_reservation(inode->i_sb, &reservation);
    return ret;
}

/*
 * Keep inline-file writes inside the shared deferred native transaction.
 *
 * The old bridge flushed the pending transaction and entered the synchronous
 * legacy inline writer for every tiny write.  Create/write loops therefore
 * paid a full checkpoint/bitmap publication per file.  Transaction-private
 * inline objects can instead be updated and repointed in the same deferred
 * transaction as their namespace creation.
 */
static ssize_t infilfs_native_inline_write_iter(
    struct inode *inode, loff_t *position, struct iov_iter *from,
    size_t requested)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(inode->i_sb);
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_native_pending *pending;
    struct infilfs_native_index_change change = {0};
    struct infilfs_object_header_disk *header;
    struct infilfs_file_payload_disk *file;
    struct infilfs_data_checksum_disk *digest;
    u8 *data = NULL;
    u8 *old_object = NULL;
    u8 *new_object = NULL;
    loff_t pos = *position;
    size_t old_size = 0;
    size_t copied = 0;
    size_t new_size;
    u64 new_block = 0;
    int ret;
    int rollback;

    if (pos < 0 || (u64)pos > INFILFS_INLINE_DATA_MAX ||
        requested > INFILFS_INLINE_DATA_MAX - (size_t)pos)
        return -EOPNOTSUPP;

    pending = infilfs_native_pending_get(inode->i_sb, true);
    if (!pending)
        return -ENOMEM;
    data = kzalloc(INFILFS_INLINE_DATA_MAX, GFP_NOFS);
    old_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    new_object = kzalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!data || !old_object || !new_object) {
        ret = -ENOMEM;
        goto out_free;
    }

    mutex_lock(&sbi->write_lock);
    ret = infilfs_native_pending_begin_locked(pending);
    if (ret)
        goto out_unlock;
    ret = infilfs_native_operation_snapshot(pending);
    if (ret)
        goto out_unlock;

    ret = infilfs_rw_load_inline(inode, data, &old_size);
    if (ret)
        goto rollback_operation;
    ret = infilfs_read_object(inode->i_sb, ii->object_block,
                              INFILFS_OBJECT_FILE, ii->object_id, old_object);
    if (ret)
        goto rollback_operation;
    header = (struct infilfs_object_header_disk *)old_object;
    file = (struct infilfs_file_payload_disk *)(header + 1);
    if (le16_to_cpu(header->object_version) != INFILFS_OBJECT_VERSION_CLASSIC ||
        le32_to_cpu(file->extent_count) != 0 ||
        le64_to_cpu(file->attributes.logical_size) != old_size ||
        old_size > INFILFS_INLINE_DATA_MAX) {
        ret = -EOPNOTSUPP;
        goto rollback_operation;
    }

    copied = copy_from_iter(data + pos, requested, from);
    if (!copied) {
        ret = -EFAULT;
        goto rollback_operation;
    }
    new_size = max_t(size_t, old_size, (size_t)pos + copied);

    memcpy(new_object, old_object, sizeof(*header) + sizeof(*file));
    header = (struct infilfs_object_header_disk *)new_object;
    file = (struct infilfs_file_payload_disk *)(header + 1);
    header->generation = cpu_to_le64(pending->tx.generation);
    header->object_version = cpu_to_le16(INFILFS_OBJECT_VERSION_CLASSIC);
    file->attributes.logical_size = cpu_to_le64(new_size);
    file->attributes.modification_time_ns = cpu_to_le64(ktime_get_real_ns());
    file->attributes.change_time_ns = file->attributes.modification_time_ns;
    file->extent_count = 0;
    file->data_checksum_type = cpu_to_le32(INFILFS_CHECKSUM_SHA256);
    memset(file->checksum_head_id, 0, sizeof(file->checksum_head_id));
    digest = (struct infilfs_data_checksum_disk *)(file + 1);
    ret = infilfs_rw_inline_digest(data, new_size, digest->bytes);
    if (ret)
        goto rollback_operation;
    memcpy(digest + 1, data, new_size);
    header->payload_size = cpu_to_le32(
        sizeof(*file) + sizeof(*digest) + new_size);

    ret = infilfs_rw_finalize_object(new_object);
    if (ret)
        goto rollback_operation;
    ret = infilfs_native_store_private_or_cow(
        pending, ii->object_block, new_object, &new_block);
    if (ret)
        goto rollback_operation;

    memcpy(change.object_id, ii->object_id, 16);
    change.object_block = new_block;
    change.object_type = INFILFS_OBJECT_FILE;
    ret = infilfs_native_index_update(pending, &change, 1);
    if (ret)
        goto rollback_operation;

    infilfs_native_operation_commit(pending);
    ii->object_block = new_block;
    i_size_write(inode, new_size);
    pending->tx.next_sb.generation = cpu_to_le64(pending->tx.generation);
    sbi->disk = pending->tx.next_sb;
    pending->pending_bytes += copied;

    if (pending->pending_bytes >= pending->publish_threshold)
        (void)infilfs_native_pending_commit_locked(pending);
    if (pending->active && !pending->commit_failed)
        mod_delayed_work(system_wq, &pending->idle_work,
                         INFILFS_NATIVE_IDLE_DELAY);

    *position = pos + copied;
    ret = (ssize_t)copied;
    goto out_unlock;

rollback_operation:
    if (copied)
        iov_iter_revert(from, copied);
    rollback = infilfs_native_operation_rollback(pending);
    if (rollback)
        ret = rollback;
out_unlock:
    mutex_unlock(&sbi->write_lock);
out_free:
    kfree(new_object);
    kfree(old_object);
    kfree(data);
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
        iocb->ki_pos = pos;
        ret = infilfs_native_inline_write_iter(
            inode, &iocb->ki_pos, from, requested);
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
            ret = infilfs_read_allocated_block(
                inode->i_sb, physical, data_block);
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
