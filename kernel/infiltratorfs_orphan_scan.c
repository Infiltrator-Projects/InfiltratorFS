// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Parallel crash-orphan discovery.
 *
 * The object-index snapshot is immutable input. Discovery is read-only and
 * therefore embarrassingly parallel: each worker scans a disjoint catalogue
 * range while the module-wide CPU gate enforces max(1, online CPUs - 1).
 * Actual orphan reclamation remains serialized by the caller because it
 * mutates authoritative namespace/allocation state.
 */
#include "infiltratorfs_internal.h"

struct infilfs_orphan_scan_work {
    struct work_struct work;
    struct completion done;
    struct super_block *sb;
    const struct infilfs_index_entry_disk *entries;
    struct infilfs_index_entry_disk *candidates;
    atomic64_t *candidate_next;
    u32 start;
    u32 end;
    u32 total;
    u64 recovery_generation;
    u32 files;
    int ret;
    bool queued;
};

static int infilfs_orphan_scan_range(struct infilfs_orphan_scan_work *item)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(item->sb);
    u8 *object;
    u32 i;
    int ret = 0;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;

    for (i = item->start; i < item->end;) {
        u32 end = min_t(u32, item->end, i + 256u);
        u32 j;

        down_read(&sbi->write_lock);
        for (j = i; j < end; ++j) {
            const struct infilfs_index_entry_disk *entry = &item->entries[j];
            struct infilfs_object_header_disk *header;
            struct infilfs_file_payload_disk *file;
            u64 live_block;
            u16 live_type;

            if (le16_to_cpu(entry->object_type) != INFILFS_OBJECT_FILE)
                continue;

            live_block = le64_to_cpu(entry->object_block);
            live_type = INFILFS_OBJECT_FILE;
            ret = infilfs_read_object(
                item->sb, live_block, INFILFS_OBJECT_FILE,
                entry->object_id, object);
            if (ret) {
                ret = infilfs_index_lookup(
                    item->sb, entry->object_id, &live_block, &live_type);
                if (ret == -ENOENT) {
                    ret = 0;
                    continue;
                }
                if (ret)
                    break;
                if (live_type != INFILFS_OBJECT_FILE) {
                    ret = -EFSCORRUPTED;
                    break;
                }
                ret = infilfs_read_object(
                    item->sb, live_block, INFILFS_OBJECT_FILE,
                    entry->object_id, object);
                if (ret)
                    break;
            }
            item->files++;

            header = (struct infilfs_object_header_disk *)object;
            if (le32_to_cpu(header->payload_size) < sizeof(*file)) {
                ret = -EFSCORRUPTED;
                break;
            }
            file = (struct infilfs_file_payload_disk *)(header + 1);
            if (le64_to_cpu(file->attributes.link_count) == 0 &&
                le64_to_cpu(header->generation) <=
                    item->recovery_generation) {
                s64 slot = atomic64_fetch_inc(item->candidate_next);

                if (slot < 0 || (u64)slot >= item->total) {
                    ret = -EFSCORRUPTED;
                    break;
                }
                item->candidates[slot] = *entry;
                item->candidates[slot].object_block =
                    cpu_to_le64(live_block);
                item->candidates[slot].object_type =
                    cpu_to_le16(INFILFS_OBJECT_FILE);
            }
        }
        up_read(&sbi->write_lock);
        if (ret)
            break;
        i = end;
        cond_resched();
    }

    kfree(object);
    return ret;
}

static void infilfs_orphan_scan_workfn(struct work_struct *work)
{
    struct infilfs_orphan_scan_work *item =
        container_of(work, struct infilfs_orphan_scan_work, work);

    infilfs_cpu_work_enter();
    item->ret = infilfs_orphan_scan_range(item);
    infilfs_cpu_work_exit();
    complete(&item->done);
}

int infilfs_orphan_discover_parallel(
    struct super_block *sb,
    const struct infilfs_index_entry_disk *entries, u32 count,
    u64 recovery_generation,
    struct infilfs_index_entry_disk **candidates_out,
    u32 *candidate_count_out, u32 *files_out)
{
    struct infilfs_orphan_scan_work *work = NULL;
    struct infilfs_index_entry_disk *candidates = NULL;
    atomic64_t candidate_next = ATOMIC64_INIT(0);
    unsigned int workers;
    u32 cursor = 0;
    u32 files = 0;
    unsigned int i;
    int ret = 0;

    if (!sb || !entries || !candidates_out || !candidate_count_out ||
        !files_out)
        return -EINVAL;
    *candidates_out = NULL;
    *candidate_count_out = 0;
    *files_out = 0;
    if (!count)
        return 0;

    workers = min_t(unsigned int, infilfs_cpu_budget(), count);
    workers = max_t(unsigned int, workers, 1u);
    work = kcalloc(workers, sizeof(*work), GFP_NOFS);
    candidates = kvmalloc_array(count, sizeof(*candidates), GFP_NOFS);
    if (!work || !candidates) {
        ret = -ENOMEM;
        goto out;
    }

    for (i = 0; i < workers; ++i) {
        u32 remaining = count - cursor;
        u32 slots = workers - i;
        u32 span = DIV_ROUND_UP(remaining, slots);

        INIT_WORK(&work[i].work, infilfs_orphan_scan_workfn);
        init_completion(&work[i].done);
        work[i].sb = sb;
        work[i].entries = entries;
        work[i].candidates = candidates;
        work[i].candidate_next = &candidate_next;
        work[i].start = cursor;
        work[i].end = cursor + span;
        work[i].total = count;
        work[i].recovery_generation = recovery_generation;
        cursor += span;
    }

    /* Queue peers first, then let the current recovery worker participate. */
    for (i = 1; i < workers; ++i)
        work[i].queued = infilfs_queue_cpu_work(&work[i].work);

    infilfs_cpu_work_enter();
    work[0].ret = infilfs_orphan_scan_range(&work[0]);
    infilfs_cpu_work_exit();

    for (i = 1; i < workers; ++i) {
        if (!work[i].queued) {
            infilfs_cpu_work_enter();
            work[i].ret = infilfs_orphan_scan_range(&work[i]);
            infilfs_cpu_work_exit();
        } else {
            wait_for_completion(&work[i].done);
        }
    }

    for (i = 0; i < workers; ++i) {
        files += work[i].files;
        if (!ret && work[i].ret)
            ret = work[i].ret;
    }
    if (!ret) {
        s64 found = atomic64_read(&candidate_next);

        if (found < 0 || (u64)found > count) {
            ret = -EFSCORRUPTED;
        } else {
            *candidates_out = candidates;
            *candidate_count_out = (u32)found;
            *files_out = files;
            candidates = NULL;
        }
    }

out:
    kvfree(candidates);
    kfree(work);
    return ret;
}
