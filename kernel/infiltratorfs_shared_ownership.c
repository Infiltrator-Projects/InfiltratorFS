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

static int infilfs_shared_read_file_extents(
    struct super_block *sb, u64 object_block, const u8 object_id[16],
    struct infilfs_extent_disk **extents_out, u32 *count_out)
{
    u8 *object = NULL, *page_block = NULL;
    struct infilfs_object_header_disk *header;
    struct infilfs_file_payload_disk *file;
    struct infilfs_extent_disk *extents = NULL;
    u32 count, copied = 0;
    int ret;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;
    ret = infilfs_read_object(sb, object_block, INFILFS_OBJECT_FILE,
                              object_id, object);
    if (ret)
        goto out;
    header = (struct infilfs_object_header_disk *)object;
    file = (struct infilfs_file_payload_disk *)(header + 1);
    count = le32_to_cpu(file->extent_count);
    if (!count) {
        *extents_out = NULL;
        *count_out = 0;
        ret = 0;
        goto out;
    }
    extents = kvmalloc_array(count, sizeof(*extents), GFP_NOFS);
    if (!extents) {
        ret = -ENOMEM;
        goto out;
    }
    if (le16_to_cpu(header->object_version) == INFILFS_OBJECT_VERSION_CLASSIC) {
        size_t need = sizeof(*file) + (size_t)count * sizeof(*extents);
        if (need != le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        memcpy(extents, file + 1, (size_t)count * sizeof(*extents));
        copied = count;
    } else if (le16_to_cpu(header->object_version) ==
                   INFILFS_OBJECT_VERSION_PAGED ||
               le16_to_cpu(header->object_version) ==
                   INFILFS_OBJECT_VERSION_TREE) {
        u32 page_count = 0, p;

        ret = infilfs_extent_layout_validate(sb, object, &page_count);
        if (ret)
            goto out;
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!page_block) {
            ret = -ENOMEM;
            goto out;
        }
        for (p = 0; p < page_count; ++p) {
            struct infilfs_metadata_page_disk *page;
            u32 n;
            u64 block;

            ret = infilfs_extent_page_block(sb, object, p, &block);
            if (ret)
                goto out;
            ret = infilfs_read_allocated_block(sb, block, page_block);
            if (ret)
                goto out;
            if (!infilfs_metadata_page_valid(
                    sb, page_block, infilfs_extent_page_magic, object_id)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            page = (struct infilfs_metadata_page_disk *)page_block;
            n = le32_to_cpu(page->entry_count);
            if (!n || n > INFILFS_EXTENTS_PER_PAGE || copied > count ||
                n > count - copied ||
                le32_to_cpu(page->bytes_used) != n * sizeof(*extents)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            memcpy(extents + copied, page + 1, (size_t)n * sizeof(*extents));
            copied += n;
        }
    } else {
        ret = -EFSCORRUPTED;
        goto out;
    }
    if (copied != count) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    *extents_out = extents;
    *count_out = count;
    extents = NULL;
    ret = 0;
out:
    kvfree(extents);
    kfree(page_block);
    kfree(object);
    return ret;
}



/*
 * Volatile shared-range accelerator.
 *
 * The exact ownership scan below is authoritative but historically made every
 * unshared unlink walk every other live file. Build a compact sweep-line index
 * once from the same authoritative live-file extents. Only physical ranges
 * observed with at least two live file owners are retained. Absence from this
 * index therefore proves a range was unshared when the index was built.
 *
 * Ordinary CoW writes, truncates, deletes and defrag only retire references and
 * allocate fresh unique ranges. They can make this index over-conservative
 * (a formerly shared range may become unique) but cannot create a false
 * negative. A possibly shared range still uses the exact scan. Native
 * reflink/clone creation is the operation that introduces a second live owner;
 * it invalidates this index before the transaction becomes visible so the next
 * ownership query rebuilds from authoritative metadata. Snapshots are protected
 * independently by the retained-snapshot allocation
 * union used when deferred frees are applied.
 */
struct infilfs_shared_event {
    u64 position;
    s8 delta;
};

static int infilfs_shared_event_compare(const void *a, const void *b)
{
    const struct infilfs_shared_event *left = a;
    const struct infilfs_shared_event *right = b;

    if (left->position < right->position)
        return -1;
    if (left->position > right->position)
        return 1;
    return 0;
}

static int infilfs_shared_event_append(
    struct infilfs_shared_event **events, size_t *count, size_t *capacity,
    u64 position, s8 delta)
{
    struct infilfs_shared_event *grown;
    size_t next;

    if (*count == *capacity) {
        next = *capacity ? *capacity * 2u : 256u;
        if (next < *capacity || next > SIZE_MAX / sizeof(*grown))
            return -EOVERFLOW;
        grown = kvmalloc_array(next, sizeof(*grown), GFP_NOFS);
        if (!grown)
            return -ENOMEM;
        if (*count)
            memcpy(grown, *events, *count * sizeof(*grown));
        kvfree(*events);
        *events = grown;
        *capacity = next;
    }
    (*events)[*count].position = position;
    (*events)[*count].delta = delta;
    (*count)++;
    return 0;
}

int infilfs_shared_ownership_index_build(
    struct infilfs_native_pending *pending)
{
    struct infilfs_index_entry_disk *entries = NULL;
    struct infilfs_shared_event *events = NULL;
    struct infilfs_native_shared_range *ranges = NULL;
    size_t event_count = 0, event_capacity = 0;
    size_t range_count = 0, range_capacity = 0;
    s64 coverage = 0;
    u32 count = 0, i;
    size_t e = 0;
    u64 previous = 0;
    int ret;

    if (pending->shared_range_index_valid)
        return 0;

    ret = infilfs_ns_index_snapshot(pending->sb, &entries, &count);
    if (ret)
        goto out;

    for (i = 0; i < count; ++i) {
        struct infilfs_extent_disk *extents = NULL;
        u32 extent_count = 0, j;

        if (le16_to_cpu(entries[i].object_type) != INFILFS_OBJECT_FILE)
            continue;
        ret = infilfs_shared_read_file_extents(
            pending->sb, le64_to_cpu(entries[i].object_block),
            entries[i].object_id, &extents, &extent_count);
        if (ret) {
            kvfree(extents);
            goto out;
        }
        for (j = 0; j < extent_count; ++j) {
            u32 flags = le32_to_cpu(extents[j].flags);
            u32 logical_blocks = le32_to_cpu(extents[j].block_count);
            u64 physical;
            u64 blocks;
            u64 end;

            if (infilfs_extent_kind(flags) != INFILFS_EXTENT_NORMAL)
                continue;
            physical = le64_to_cpu(extents[j].physical_block);
            blocks = infilfs_extent_physical_blocks(logical_blocks, flags);
            if (!physical || !blocks || physical > U64_MAX - blocks) {
                ret = -EFSCORRUPTED;
                break;
            }
            end = physical + blocks;
            ret = infilfs_shared_event_append(
                &events, &event_count, &event_capacity, physical, 1);
            if (!ret)
                ret = infilfs_shared_event_append(
                    &events, &event_count, &event_capacity, end, -1);
            if (ret)
                break;
        }
        kvfree(extents);
        if (ret)
            goto out;
    }

    if (!event_count) {
        kvfree(pending->shared_ranges);
        pending->shared_ranges = NULL;
        pending->shared_range_count = 0;
        pending->shared_range_index_valid = true;
        ret = 0;
        goto out;
    }

    sort(events, event_count, sizeof(*events),
         infilfs_shared_event_compare, NULL);
    previous = events[0].position;
    while (e < event_count) {
        u64 position = events[e].position;
        s64 delta = 0;

        if (position < previous) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        if (position > previous && coverage >= 2) {
            if (coverage > U32_MAX) {
                ret = -EOVERFLOW;
                goto out;
            }
            ret = infilfs_shared_range_append(
                &ranges, &range_count, &range_capacity,
                previous, position, (u32)coverage);
            if (ret)
                goto out;
        }
        while (e < event_count && events[e].position == position) {
            delta += events[e].delta;
            e++;
        }
        if ((delta < 0 && coverage < -delta) ||
            (delta > 0 && coverage > S64_MAX - delta)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        coverage += delta;
        if (coverage < 0) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        previous = position;
    }
    if (coverage != 0) {
        ret = -EFSCORRUPTED;
        goto out;
    }

    kvfree(pending->shared_ranges);
    pending->shared_ranges = ranges;
    pending->shared_range_count = range_count;
    pending->shared_range_index_valid = true;
    ranges = NULL;
    ret = 0;
out:
    kvfree(ranges);
    kvfree(events);
    kvfree(entries);
    if (ret) {
        pending->shared_range_index_valid = false;
        pending->shared_range_count = 0;
    }
    return ret;
}

/*
 * Ownership discovery may scan every live file. Serialize builders, but take
 * only write_lock read-side so an eviction cannot monopolize topology access.
 */
int infilfs_shared_ownership_prepare_index(
    struct infilfs_native_pending *pending)
{
    struct infilfs_sb_info *sbi;
    int ret = 0;

    if (!pending || !pending->sb)
        return -EINVAL;
    if (READ_ONCE(pending->shared_range_index_valid))
        return 0;

    sbi = INFILFS_SB(pending->sb);
    if (!sbi)
        return -EIO;

    mutex_lock(&sbi->shared_range_build_lock);
    if (!READ_ONCE(pending->shared_range_index_valid)) {
        down_read(&sbi->write_lock);
        ret = infilfs_shared_ownership_index_build(pending);
        up_read(&sbi->write_lock);
    }
    mutex_unlock(&sbi->shared_range_build_lock);
    return ret;
}

bool infilfs_shared_ownership_maybe_shared(
    const struct infilfs_native_pending *pending, u64 start, u64 end)
{
    size_t low = 0;
    size_t high = pending->shared_range_count;

    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        const struct infilfs_native_shared_range *range =
            &pending->shared_ranges[middle];

        if (range->end <= start)
            low = middle + 1u;
        else
            high = middle;
    }
    return low < pending->shared_range_count &&
        pending->shared_ranges[low].start < end;
}

int infilfs_shared_ownership_other_reference_cover(
    struct super_block *sb, const u8 owner_id[16], u64 cursor, u64 end,
    u64 *cover_end, u64 *next_start)
{
    struct infilfs_index_entry_disk *entries = NULL;
    u32 count = 0, i;
    int ret;

    *cover_end = cursor;
    *next_start = end;
    ret = infilfs_ns_index_snapshot(sb, &entries, &count);
    if (ret)
        return ret;
    for (i = 0; i < count; ++i) {
        struct infilfs_extent_disk *extents = NULL;
        u32 extent_count = 0, j;
        if (le16_to_cpu(entries[i].object_type) != INFILFS_OBJECT_FILE ||
            memcmp(entries[i].object_id, owner_id, 16) == 0)
            continue;
        ret = infilfs_shared_read_file_extents(
            sb, le64_to_cpu(entries[i].object_block), entries[i].object_id,
            &extents, &extent_count);
        if (ret) {
            kvfree(entries);
            return ret;
        }
        for (j = 0; j < extent_count; ++j) {
            u64 physical, extent_end, blocks;
            u32 flags = le32_to_cpu(extents[j].flags);
            u32 logical_blocks = le32_to_cpu(extents[j].block_count);

            if (infilfs_extent_kind(flags) != INFILFS_EXTENT_NORMAL)
                continue;
            physical = le64_to_cpu(extents[j].physical_block);
            blocks = infilfs_extent_physical_blocks(logical_blocks, flags);
            extent_end = physical + blocks;
            if (extent_end <= cursor || physical >= end)
                continue;
            if (physical <= cursor) {
                u64 candidate = min_t(u64, extent_end, end);
                if (candidate > *cover_end)
                    *cover_end = candidate;
            } else if (physical < *next_start) {
                *next_start = physical;
            }
        }
        kvfree(extents);
    }
    kvfree(entries);
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
