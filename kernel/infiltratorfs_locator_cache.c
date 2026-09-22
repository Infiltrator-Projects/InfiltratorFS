// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"

/*
 * Volatile native lookup accelerators.
 *
 * These structures cache object-index positions, directory names and the most
 * recently used checksum-writer tail. They are rebuildable session state:
 * persistent format meaning and crash recovery never depend on their contents.
 * Keeping them in a compiled object prevents the data-path compositor from
 * owning unrelated hash-table mechanics.
 */

#define INFILFS_NATIVE_INDEX_LOCATOR_MIN_CAPACITY 64u
#define INFILFS_NATIVE_DIRECTORY_LOCATOR_MIN_CAPACITY 64u

u32 infilfs_native_id_hash(const u8 id[16])
{
    u32 h = 2166136261u;
    unsigned int i;

    for (i = 0; i < 16; ++i) {
        h ^= id[i];
        h *= 16777619u;
    }
    return h;
}

void infilfs_native_writer_tail_invalidate(
    struct infilfs_native_pending *pending)
{
    memset(pending->writer_tail, 0, sizeof(pending->writer_tail));
}

bool infilfs_native_writer_tail_lookup(
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

void infilfs_native_writer_tail_store(
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

void infilfs_native_index_locator_invalidate(
    struct infilfs_native_pending *pending)
{
    pending->index_locator_valid = false;
}

static u32 infilfs_native_name_hash(struct super_block *sb,
                                    const char *name, size_t length)
{
    return infilfs_name_hash(sb, (const u8 *)name, length);
}

void infilfs_native_directory_locator_invalidate(
    struct infilfs_native_pending *pending)
{
    pending->directory_locator_valid = false;
    pending->directory_locator_count = 0;
    pending->directory_locator_names_bytes = 0;
    memset(pending->directory_locator_owner_id, 0,
           sizeof(pending->directory_locator_owner_id));
}

static int infilfs_native_directory_locator_names_reserve(
    struct infilfs_native_pending *pending, size_t extra)
{
    u8 *fresh;
    size_t needed;
    size_t capacity;

    if (extra > (size_t)-1 - pending->directory_locator_names_bytes)
        return -EOVERFLOW;
    needed = pending->directory_locator_names_bytes + extra;
    if (needed <= pending->directory_locator_names_capacity)
        return 0;

    capacity = pending->directory_locator_names_capacity;
    if (capacity < 4096u)
        capacity = 4096u;
    while (capacity < needed) {
        if (capacity > (size_t)-1 / 2u) {
            capacity = needed;
            break;
        }
        capacity <<= 1;
    }
    fresh = kvmalloc(capacity, GFP_NOFS);
    if (!fresh)
        return -ENOMEM;
    if (pending->directory_locator_names_bytes)
        memcpy(fresh, pending->directory_locator_names,
               pending->directory_locator_names_bytes);
    kvfree(pending->directory_locator_names);
    pending->directory_locator_names = fresh;
    pending->directory_locator_names_capacity = capacity;
    return 0;
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

int infilfs_native_directory_locator_ensure(
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

bool infilfs_native_directory_locator_matches(
    struct infilfs_native_pending *pending, const u8 owner_id[16], u32 count)
{
    return pending->directory_locator_valid &&
        pending->directory_locator_count == count &&
        memcmp(pending->directory_locator_owner_id, owner_id, 16) == 0;
}

bool infilfs_native_directory_locator_lookup(
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

    hash = infilfs_native_name_hash(pending->sb, name, name_len);
    slot = hash & (pending->directory_locator_capacity - 1u);
    for (probes = 0; probes < pending->directory_locator_capacity; ++probes) {
        struct infilfs_native_directory_locator *entry =
            &pending->directory_locators[
                (slot + probes) & (pending->directory_locator_capacity - 1u)];

        if (!entry->valid)
            return false;
        if (entry->hash == hash && entry->name_len == name_len &&
            entry->name_offset <= pending->directory_locator_names_bytes &&
            name_len <= pending->directory_locator_names_bytes -
                entry->name_offset &&
            memcmp(pending->directory_locator_names + entry->name_offset,
                   name, name_len) == 0)
            return true;
    }
    return false;
}

int infilfs_native_directory_locator_insert(
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

    hash = infilfs_native_name_hash(pending->sb, name, name_len);
    slot = hash & (pending->directory_locator_capacity - 1u);
    for (probes = 0; probes < pending->directory_locator_capacity; ++probes) {
        entry = &pending->directory_locators[
            (slot + probes) & (pending->directory_locator_capacity - 1u)];
        if (entry->valid) {
            if (entry->hash == hash && entry->name_len == name_len &&
                entry->name_offset <= pending->directory_locator_names_bytes &&
                name_len <= pending->directory_locator_names_bytes -
                    entry->name_offset &&
                memcmp(pending->directory_locator_names + entry->name_offset,
                       name, name_len) == 0)
                return -EEXIST;
            continue;
        }
        ret = infilfs_native_directory_locator_names_reserve(
            pending, name_len);
        if (ret)
            return ret;
        memset(entry, 0, sizeof(*entry));
        entry->name_offset = pending->directory_locator_names_bytes;
        entry->hash = hash;
        entry->name_len = (u16)name_len;
        memcpy(pending->directory_locator_names + entry->name_offset,
               name, name_len);
        pending->directory_locator_names_bytes += name_len;
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

int infilfs_native_index_locator_ensure(
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

int infilfs_native_index_locator_insert(
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

bool infilfs_native_index_locator_lookup(
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

int infilfs_native_index_locator_build(
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

