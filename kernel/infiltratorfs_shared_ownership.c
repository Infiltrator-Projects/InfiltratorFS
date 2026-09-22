// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"

static int infilfs_shared_range_compare(const void *left, const void *right)
{
    const struct infilfs_native_shared_range *a = left;
    const struct infilfs_native_shared_range *b = right;

    if (a->start < b->start)
        return -1;
    if (a->start > b->start)
        return 1;
    if (a->end < b->end)
        return -1;
    if (a->end > b->end)
        return 1;
    return 0;
}

static int infilfs_shared_range_append(
    struct infilfs_native_shared_range **ranges, size_t *count,
    size_t *capacity, u64 start, u64 end, u32 refs)
{
    struct infilfs_native_shared_range *grown;
    size_t next;

    if (start >= end || refs < 2u)
        return -EFSCORRUPTED;
    if (*count) {
        struct infilfs_native_shared_range *last = &(*ranges)[*count - 1u];

        if (start < last->end)
            return -EFSCORRUPTED;
        if (start == last->end && last->refs == refs) {
            last->end = end;
            return 0;
        }
    }
    if (*count == *capacity) {
        next = *capacity ? *capacity * 2u : 64u;
        if (next < *capacity || next > SIZE_MAX / sizeof(*grown))
            return -EOVERFLOW;
        grown = kvmalloc_array(next, sizeof(*grown), GFP_NOFS);
        if (!grown)
            return -ENOMEM;
        if (*count)
            memcpy(grown, *ranges, *count * sizeof(*grown));
        kvfree(*ranges);
        *ranges = grown;
        *capacity = next;
    }
    (*ranges)[*count].start = start;
    (*ranges)[*count].end = end;
    (*ranges)[*count].refs = refs;
    (*count)++;
    return 0;
}

static int infilfs_shared_owned_ranges_from_extents(
    const struct infilfs_extent_disk *extents, u32 extent_count,
    struct infilfs_native_shared_range **owned_out, size_t *owned_count_out)
{
    struct infilfs_native_shared_range *owned = NULL;
    size_t owned_count = 0;
    size_t write = 0;
    u32 i;

    *owned_out = NULL;
    *owned_count_out = 0;
    if (!extent_count)
        return 0;
    if (!extents)
        return -EINVAL;

    owned = kvmalloc_array(extent_count, sizeof(*owned), GFP_NOFS);
    if (!owned)
        return -ENOMEM;

    for (i = 0; i < extent_count; ++i) {
        u32 flags = le32_to_cpu(extents[i].flags);
        u32 logical_blocks = le32_to_cpu(extents[i].block_count);
        u64 start;
        u64 blocks;

        if (infilfs_extent_kind(flags) != INFILFS_EXTENT_NORMAL)
            continue;
        start = le64_to_cpu(extents[i].physical_block);
        blocks = infilfs_extent_physical_blocks(logical_blocks, flags);
        if (!start || !blocks || start > U64_MAX - blocks) {
            kvfree(owned);
            return -EFSCORRUPTED;
        }
        owned[owned_count].start = start;
        owned[owned_count].end = start + blocks;
        owned[owned_count].refs = 1u;
        owned_count++;
    }
    if (!owned_count) {
        kvfree(owned);
        return 0;
    }

    sort(owned, owned_count, sizeof(*owned),
         infilfs_shared_range_compare, NULL);
    for (i = 0; i < owned_count; ++i) {
        if (write && owned[i].start <= owned[write - 1u].end) {
            if (owned[i].end > owned[write - 1u].end)
                owned[write - 1u].end = owned[i].end;
            continue;
        }
        owned[write++] = owned[i];
    }

    *owned_out = owned;
    *owned_count_out = write;
    return 0;
}

/*
 * Add one live owner to a valid volatile shared-range reference index.
 * Reflink is the mutation that can turn a private physical range into a shared
 * one, so maintaining multiplicity here avoids clone -> truncate rebuilding
 * ownership by walking every live file.
 */
int infilfs_shared_ownership_add_owner(
    struct infilfs_native_pending *pending,
    const struct infilfs_extent_disk *extents, u32 extent_count)
{
    struct infilfs_native_shared_range *owned = NULL;
    struct infilfs_native_shared_range *updated = NULL;
    size_t owned_count = 0;
    size_t updated_count = 0;
    size_t updated_capacity = 0;
    size_t shared_index = 0;
    size_t owned_index = 0;
    bool shared_active = false;
    bool owned_active = false;
    u64 position;
    int ret;

    if (!pending || !pending->shared_range_index_valid)
        return 0;

    ret = infilfs_shared_owned_ranges_from_extents(
        extents, extent_count, &owned, &owned_count);
    if (ret || !owned_count)
        goto out;
    if (pending->shared_range_count && !pending->shared_ranges) {
        ret = -EFSCORRUPTED;
        goto out;
    }

    position = pending->shared_range_count ?
        pending->shared_ranges[0].start : owned[0].start;
    if (owned[0].start < position)
        position = owned[0].start;

    while (shared_index < pending->shared_range_count ||
           owned_index < owned_count || shared_active || owned_active) {
        const struct infilfs_native_shared_range *shared = NULL;
        u64 next = U64_MAX;
        u32 refs = 0;

        if (!shared_active &&
            shared_index < pending->shared_range_count &&
            pending->shared_ranges[shared_index].start == position) {
            shared = &pending->shared_ranges[shared_index];
            if (shared->start >= shared->end || shared->refs < 2u ||
                (shared_index &&
                 shared->start <
                    pending->shared_ranges[shared_index - 1u].end)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            shared_active = true;
        }
        if (!owned_active && owned_index < owned_count &&
            owned[owned_index].start == position)
            owned_active = true;

        if (shared_active) {
            shared = &pending->shared_ranges[shared_index];
            next = min_t(u64, next, shared->end);
        } else if (shared_index < pending->shared_range_count) {
            next = min_t(
                u64, next, pending->shared_ranges[shared_index].start);
        }
        if (owned_active)
            next = min_t(u64, next, owned[owned_index].end);
        else if (owned_index < owned_count)
            next = min_t(u64, next, owned[owned_index].start);

        if (next == U64_MAX)
            break;
        if (next < position) {
            ret = -EFSCORRUPTED;
            goto out;
        }

        if (next > position) {
            if (owned_active) {
                if (shared_active) {
                    if (shared->refs == U32_MAX) {
                        ret = -EOVERFLOW;
                        goto out;
                    }
                    refs = shared->refs + 1u;
                } else {
                    refs = 2u;
                }
            } else if (shared_active) {
                refs = shared->refs;
            }
            if (refs) {
                ret = infilfs_shared_range_append(
                    &updated, &updated_count, &updated_capacity,
                    position, next, refs);
                if (ret)
                    goto out;
            }
        }

        position = next;
        if (shared_active &&
            position == pending->shared_ranges[shared_index].end) {
            shared_active = false;
            shared_index++;
        }
        if (owned_active && position == owned[owned_index].end) {
            owned_active = false;
            owned_index++;
        }
        if (!shared_active && !owned_active) {
            u64 next_start = U64_MAX;

            if (shared_index < pending->shared_range_count)
                next_start = min_t(
                    u64, next_start,
                    pending->shared_ranges[shared_index].start);
            if (owned_index < owned_count)
                next_start = min_t(
                    u64, next_start, owned[owned_index].start);
            if (next_start != U64_MAX)
                position = next_start;
        }
    }

    kvfree(pending->shared_ranges);
    pending->shared_ranges = updated;
    pending->shared_range_count = updated_count;
    updated = NULL;
out:
    kvfree(updated);
    kvfree(owned);
    return ret;
}

/*
 * Remove one live file owner from the volatile shared-range reference index.
 * The index stores only intervals with at least two owners. Intervals whose
 * count falls from two to one disappear; counts above two are decremented.
 * This keeps bulk final-unlink O(shared intervals + file extents) instead of
 * rebuilding the complete live-file ownership map after every inode eviction.
 */
int infilfs_shared_ownership_drop_owner(
    struct infilfs_native_pending *pending, struct inode *inode)
{
    struct infilfs_native_shared_range *owned = NULL;
    struct infilfs_native_shared_range *updated = NULL;
    struct infilfs_extent_disk *extents = NULL;
    u8 *object = NULL;
    size_t updated_count = 0;
    size_t updated_capacity = 0;
    size_t owned_count = 0;
    size_t owner_index = 0;
    u32 extent_count = 0;
    u64 old_blocks = 0;
    bool was_inline = false;
    size_t range_index;
    int ret = 0;

    if (!pending || !inode || !pending->shared_range_index_valid ||
        !pending->shared_range_count)
        return 0;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;
    ret = infilfs_native_collect_extents(
        pending, inode, object, &extents, &extent_count,
        &old_blocks, &was_inline);
    if (ret)
        goto out;
    (void)old_blocks;
    (void)was_inline;

    ret = infilfs_shared_owned_ranges_from_extents(
        extents, extent_count, &owned, &owned_count);
    if (ret || !owned_count)
        goto out;

    for (range_index = 0; range_index < pending->shared_range_count;
         ++range_index) {
        const struct infilfs_native_shared_range *shared =
            &pending->shared_ranges[range_index];
        size_t k;
        u64 cursor;

        if (shared->start >= shared->end || shared->refs < 2u) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        cursor = shared->start;
        while (owner_index < owned_count &&
               owned[owner_index].end <= cursor)
            owner_index++;

        k = owner_index;
        while (k < owned_count && owned[k].start < shared->end) {
            u64 overlap_end;

            if (owned[k].end <= cursor) {
                k++;
                continue;
            }
            if (owned[k].start > cursor) {
                u64 gap_end = min_t(u64, owned[k].start, shared->end);

                ret = infilfs_shared_range_append(
                    &updated, &updated_count, &updated_capacity,
                    cursor, gap_end, shared->refs);
                if (ret)
                    goto out;
                cursor = gap_end;
                if (cursor >= shared->end)
                    break;
            }

            overlap_end = min_t(u64, owned[k].end, shared->end);
            if (overlap_end > cursor) {
                if (shared->refs > 2u) {
                    ret = infilfs_shared_range_append(
                        &updated, &updated_count, &updated_capacity,
                        cursor, overlap_end, shared->refs - 1u);
                    if (ret)
                        goto out;
                }
                cursor = overlap_end;
            }
            if (owned[k].end <= cursor)
                k++;
            else
                break;
        }
        if (cursor < shared->end) {
            ret = infilfs_shared_range_append(
                &updated, &updated_count, &updated_capacity,
                cursor, shared->end, shared->refs);
            if (ret)
                goto out;
        }
        owner_index = k;
    }

    kvfree(pending->shared_ranges);
    pending->shared_ranges = updated;
    pending->shared_range_count = updated_count;
    updated = NULL;
out:
    kvfree(updated);
    kvfree(owned);
    kvfree(extents);
    kfree(object);
    return ret;
}
