// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"

#define INFILFS_NATIVE_CHECKSUM_CACHE_SLOTS 256u
#define INFILFS_NATIVE_CHECKSUM_GROUP_CACHE_SLOTS 4096u

static DEFINE_SPINLOCK(infilfs_native_checksum_cache_lock);
static struct infilfs_native_checksum_cache_entry
    infilfs_native_checksum_cache[INFILFS_NATIVE_CHECKSUM_CACHE_SLOTS];
static struct infilfs_native_checksum_cache_entry
    infilfs_native_checksum_group_cache[
        INFILFS_NATIVE_CHECKSUM_GROUP_CACHE_SLOTS];

static u32 infilfs_checksum_cache_id_hash(const u8 id[16])
{
    u32 h = 2166136261u;
    unsigned int i;

    for (i = 0; i < 16; ++i) {
        h ^= id[i];
        h *= 16777619u;
    }
    return h;
}

void infilfs_native_checksum_cache_invalidate_sb(struct super_block *sb)
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

bool infilfs_native_checksum_cache_lookup(
    struct super_block *sb, const u8 owner_id[16],
    struct infilfs_native_checksum_cache_entry *out)
{
    unsigned long flags;
    u32 slot = infilfs_checksum_cache_id_hash(owner_id) &
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

void infilfs_native_checksum_cache_store(
    struct super_block *sb, const u8 owner_id[16], const u8 object_id[16],
    u64 object_block, u64 start_logical)
{
    unsigned long flags;
    u32 slot = infilfs_checksum_cache_id_hash(owner_id) &
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
    u32 h = infilfs_checksum_cache_id_hash(owner_id);

    h ^= (u32)start_logical;
    h *= 16777619u;
    h ^= (u32)(start_logical >> 32);
    h *= 16777619u;
    return h & (INFILFS_NATIVE_CHECKSUM_GROUP_CACHE_SLOTS - 1u);
}

bool infilfs_native_checksum_group_cache_lookup(
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

void infilfs_native_checksum_group_cache_store(
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

