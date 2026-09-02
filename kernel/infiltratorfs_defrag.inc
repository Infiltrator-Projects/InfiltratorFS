/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Native mounted fragmentation metrics and bounded online file defragmentation.
 *
 * Defragmentation is copy-on-write. Uncompressed adjacent extents can be
 * coalesced into one extent. Mixed/compressed runs are relocated as opaque
 * stored streams into one contiguous physical allocation while preserving
 * every logical boundary, codec identifier and stored-byte count. The old
 * blocks are released through the existing snapshot/reflink-aware path.
 * Compressed streams are therefore never sliced, decompressed or recompressed
 * merely to improve physical placement.
 */
#include <linux/math64.h>
#include <linux/uaccess.h>

#define INFILFS_DEFRAG_DEFAULT_BYTES (64ULL * 1024ULL * 1024ULL)
#define INFILFS_DEFRAG_MAX_BYTES     (512ULL * 1024ULL * 1024ULL)

static void infilfs_fragmentation_from_extents(
    const struct infilfs_extent_disk *extents, u32 count, u64 logical_bytes,
    u64 generation, struct infilfs_fragmentation_metrics *metrics)
{
    u64 allocated = 0;
    u64 data_extents = 0;
    u64 hole_extents = 0;
    u64 largest = 0;
    u32 i;

    memset(metrics, 0, sizeof(*metrics));
    metrics->logical_bytes = logical_bytes;
    metrics->generation = generation;

    for (i = 0; i < count; ++i) {
        u64 blocks = le32_to_cpu(extents[i].block_count);
        u32 flags = le32_to_cpu(extents[i].flags);

        if (infilfs_extent_kind(flags) == INFILFS_EXTENT_NORMAL) {
            u64 physical_blocks =
                infilfs_extent_physical_blocks((u32)blocks, flags);

            allocated += physical_blocks;
            data_extents++;
            largest = max_t(u64, largest, physical_blocks);
        } else if (infilfs_extent_kind(flags) == INFILFS_EXTENT_HOLE) {
            hole_extents++;
        }
    }

    metrics->allocated_blocks = allocated;
    metrics->data_extents = data_extents;
    metrics->hole_extents = hole_extents;
    metrics->largest_data_extent_blocks = largest;
    if (data_extents > 1)
        metrics->fragmentation_milli =
            div64_u64((data_extents - 1u) * 1000u, data_extents);
}

static int infilfs_fragmentation_metrics_locked(
    struct inode *inode, struct infilfs_fragmentation_metrics *metrics)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(inode->i_sb);
    struct infilfs_extent_disk *extents = NULL;
    struct infilfs_object_header_disk *header;
    struct infilfs_file_payload_disk *file;
    u8 *object;
    u64 logical_blocks = 0;
    bool was_inline = false;
    u32 count = 0;
    int ret;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;

    ret = infilfs_native_collect_extents(
        NULL, inode, object, &extents, &count, &logical_blocks, &was_inline);
    if (ret)
        goto out;

    header = (struct infilfs_object_header_disk *)object;
    file = (struct infilfs_file_payload_disk *)(header + 1);
    infilfs_fragmentation_from_extents(
        extents, count, le64_to_cpu(file->attributes.logical_size),
        le64_to_cpu(sbi->disk.generation), metrics);
    (void)logical_blocks;
    (void)was_inline;

out:
    kvfree(extents);
    kfree(object);
    return ret;
}

static int infilfs_fragmentation_metrics(
    struct inode *inode, struct infilfs_fragmentation_metrics *metrics)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(inode->i_sb);
    int ret;

    if (!S_ISREG(inode->i_mode))
        return -EINVAL;
    mutex_lock(&sbi->write_lock);
    ret = infilfs_fragmentation_metrics_locked(inode, metrics);
    mutex_unlock(&sbi->write_lock);
    return ret;
}

static bool infilfs_defrag_find_candidate(
    const struct infilfs_extent_disk *extents, u32 count, u64 max_blocks,
    u32 *first_out, u32 *last_out, u64 *logical_out, u64 *blocks_out)
{
    u32 first;

    for (first = 0; first < count; ++first) {
        u64 blocks = 0;
        u32 runs = 0;
        u32 last = first;

        if (le32_to_cpu(extents[first].flags) != INFILFS_EXTENT_NORMAL)
            continue;

        while (last < count &&
               le32_to_cpu(extents[last].flags) == INFILFS_EXTENT_NORMAL) {
            u64 extent_blocks = le32_to_cpu(extents[last].block_count);

            if (extent_blocks > max_blocks - blocks)
                break;
            blocks += extent_blocks;
            runs++;
            last++;
        }

        if (runs >= 2) {
            *first_out = first;
            *last_out = last;
            *logical_out = le64_to_cpu(extents[first].logical_block);
            *blocks_out = blocks;
            return true;
        }
    }
    return false;
}

static bool infilfs_defrag_find_compressed_candidate(
    const struct infilfs_extent_disk *extents, u32 count, u64 max_blocks,
    u32 *first_out, u32 *last_out, u64 *logical_out,
    u64 *logical_blocks_out, u64 *physical_blocks_out)
{
    u32 first;

    for (first = 0; first < count; ++first) {
        u64 logical_blocks = 0;
        u64 physical_blocks = 0;
        u64 previous_physical_end = 0;
        bool have_previous = false;
        bool has_compressed = false;
        bool physically_split = false;
        u32 last = first;

        while (last < count) {
            u32 flags = le32_to_cpu(extents[last].flags);
            u32 extent_logical = le32_to_cpu(extents[last].block_count);
            u64 extent_physical;
            u64 source;

            if (infilfs_extent_kind(flags) != INFILFS_EXTENT_NORMAL)
                break;
            extent_physical =
                infilfs_extent_physical_blocks(extent_logical, flags);
            if (!extent_physical ||
                extent_physical > max_blocks - physical_blocks)
                break;
            source = le64_to_cpu(extents[last].physical_block);
            if (have_previous && source != previous_physical_end)
                physically_split = true;
            if (infilfs_extent_is_compressed(flags))
                has_compressed = true;

            physical_blocks += extent_physical;
            logical_blocks += extent_logical;
            previous_physical_end = source + extent_physical;
            have_previous = true;
            last++;
        }

        if (last - first >= 2u && has_compressed && physically_split) {
            *first_out = first;
            *last_out = last;
            *logical_out = le64_to_cpu(extents[first].logical_block);
            *logical_blocks_out = logical_blocks;
            *physical_blocks_out = physical_blocks;
            return true;
        }
    }
    return false;
}

static int infilfs_defrag_copy_candidate(
    struct infilfs_native_pending *pending,
    const struct infilfs_extent_disk *extents,
    u32 first, u32 last, u64 new_start)
{
    u8 *block;
    u64 destination = new_start;
    u32 i;
    int ret = 0;

    block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!block)
        return -ENOMEM;

    for (i = first; i < last; ++i) {
        u64 source = le64_to_cpu(extents[i].physical_block);
        u32 logical_blocks = le32_to_cpu(extents[i].block_count);
        u32 flags = le32_to_cpu(extents[i].flags);
        u64 blocks;
        u64 j;

        if (infilfs_extent_kind(flags) != INFILFS_EXTENT_NORMAL) {
            ret = -EFSCORRUPTED;
            break;
        }
        blocks = infilfs_extent_physical_blocks(logical_blocks, flags);
        if (!blocks) {
            ret = -EFSCORRUPTED;
            break;
        }

        for (j = 0; j < blocks; ++j) {
            ret = infilfs_read_allocated_block(
                pending->sb, source + j, block);
            if (ret)
                goto out;
            ret = infilfs_native_stage_block(
                pending->sb, destination, block);
            if (ret)
                goto out;
            destination++;
        }
    }

out:
    kfree(block);
    return ret;
}

static int infilfs_defrag_one(
    struct inode *inode, struct infilfs_defrag_request *request)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(inode->i_sb);
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_native_pending *pending = NULL;
    struct infilfs_extent_disk *old_extents = NULL;
    struct infilfs_extent_disk *new_extents = NULL;
    struct infilfs_object_header_disk *old_header;
    struct infilfs_file_payload_disk *old_file;
    struct infilfs_file_payload_disk working_file;
    struct infilfs_native_index_change change = {0};
    struct infilfs_fragmentation_metrics before = {0};
    struct infilfs_fragmentation_metrics after = {0};
    u8 *old_object = NULL;
    u8 *new_object = NULL;
    u64 old_blocks = 0;
    u64 segment_logical = 0;
    u64 segment_blocks = 0;
    u64 segment_physical_blocks = 0;
    u64 new_physical = 0;
    u64 new_file_block = 0;
    u64 max_bytes;
    u64 max_blocks;
    u32 old_count = 0;
    u32 new_count = 0;
    u32 first = 0;
    u32 last = 0;
    bool was_inline = false;
    bool file_repoint = false;
    bool preserve_boundaries = false;
    int ret;

    if (!S_ISREG(inode->i_mode))
        return -EINVAL;
    if (sb_rdonly(inode->i_sb))
        return -EROFS;

    max_bytes = request->max_bytes ?
        min_t(u64, request->max_bytes, INFILFS_DEFRAG_MAX_BYTES) :
        INFILFS_DEFRAG_DEFAULT_BYTES;
    max_blocks = max_bytes >> INFILFS_DISK_BLOCK_SHIFT;
    if (max_blocks < 2)
        return -EINVAL;

    ret = filemap_write_and_wait(inode->i_mapping);
    if (ret)
        return ret;

    old_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    new_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!old_object || !new_object) {
        ret = -ENOMEM;
        goto out;
    }

    ret = infilfs_ns_begin(inode->i_sb, &pending);
    if (ret)
        goto out;

    ret = infilfs_native_collect_extents(
        pending, inode, old_object, &old_extents, &old_count,
        &old_blocks, &was_inline);
    if (ret)
        goto finish;

    old_header = (struct infilfs_object_header_disk *)old_object;
    old_file = (struct infilfs_file_payload_disk *)(old_header + 1);
    infilfs_fragmentation_from_extents(
        old_extents, old_count,
        le64_to_cpu(old_file->attributes.logical_size),
        le64_to_cpu(sbi->disk.generation), &before);

    request->before_data_extents = before.data_extents;
    request->before_fragmentation_milli = before.fragmentation_milli;

    if (was_inline || before.data_extents <= 1) {
        ret = infilfs_native_operation_rollback(pending);
        mutex_unlock(&sbi->write_lock);
        pending = NULL;
        if (!ret)
            ret = infilfs_native_pending_flush_sb(inode->i_sb);
        if (!ret) {
            request->after_data_extents = before.data_extents;
            request->after_fragmentation_milli =
                before.fragmentation_milli;
            request->moved_bytes = 0;
        }
        goto out;
    }

    if (infilfs_defrag_find_candidate(
            old_extents, old_count, max_blocks,
            &first, &last, &segment_logical, &segment_blocks)) {
        segment_physical_blocks = segment_blocks;
    } else if (infilfs_defrag_find_compressed_candidate(
                   old_extents, old_count, max_blocks,
                   &first, &last, &segment_logical, &segment_blocks,
                   &segment_physical_blocks)) {
        preserve_boundaries = true;
    } else {
        ret = infilfs_native_operation_rollback(pending);
        mutex_unlock(&sbi->write_lock);
        pending = NULL;
        if (!ret)
            ret = infilfs_native_pending_flush_sb(inode->i_sb);
        if (!ret) {
            request->after_data_extents = before.data_extents;
            request->after_fragmentation_milli =
                before.fragmentation_milli;
            request->moved_bytes = 0;
        }
        goto out;
    }

    ret = infilfs_native_alloc_data_exact(
        pending, segment_physical_blocks, 0, &new_physical);
    if (ret)
        goto finish;

    ret = infilfs_defrag_copy_candidate(
        pending, old_extents, first, last, new_physical);
    if (ret)
        goto finish;

    if (!preserve_boundaries) {
        ret = infilfs_native_replace_extent_range(
            old_extents, old_count, old_blocks,
            segment_logical, segment_blocks, new_physical,
            INFILFS_EXTENT_NORMAL, &new_extents, &new_count);
        if (ret)
            goto finish;
    } else {
        u64 destination = new_physical;
        u32 i;

        new_extents = kvmalloc_array(
            old_count ? old_count : 1u, sizeof(*new_extents), GFP_NOFS);
        if (!new_extents) {
            ret = -ENOMEM;
            goto finish;
        }
        memcpy(new_extents, old_extents,
               (size_t)old_count * sizeof(*new_extents));
        new_count = old_count;
        for (i = first; i < last; ++i) {
            u32 flags = le32_to_cpu(old_extents[i].flags);
            u32 logical = le32_to_cpu(old_extents[i].block_count);
            u64 physical =
                infilfs_extent_physical_blocks(logical, flags);

            new_extents[i].physical_block = cpu_to_le64(destination);
            destination += physical;
        }
        if (destination != new_physical + segment_physical_blocks) {
            ret = -EFSCORRUPTED;
            goto finish;
        }
    }

    ret = infilfs_native_release_replaced_extents(
        pending, ii->object_id, old_extents, old_count,
        segment_logical, segment_blocks);
    if (ret)
        goto finish;

    memcpy(&working_file, old_file, sizeof(working_file));
    ret = infilfs_native_build_file_object(
        pending, inode, old_object, &working_file,
        new_extents, new_count,
        le64_to_cpu(old_file->attributes.logical_size),
        new_object, &new_file_block, &file_repoint);
    if (ret)
        goto finish;

    /*
     * Defragmentation must not look like a content modification.  The shared
     * file-object builder normally advances mtime/ctime for writes; restore
     * the original timestamps before publishing the relocated extent map.
     * The first store already captured operation rollback state when needed.
     */
    {
        struct infilfs_object_header_disk *new_header =
            (struct infilfs_object_header_disk *)new_object;
        struct infilfs_file_payload_disk *new_file =
            (struct infilfs_file_payload_disk *)(new_header + 1);

        new_file->attributes.modification_time_ns =
            old_file->attributes.modification_time_ns;
        new_file->attributes.change_time_ns =
            old_file->attributes.change_time_ns;
        ret = infilfs_rw_finalize_object(new_object);
        if (ret)
            goto finish;
        ret = infilfs_native_stage_block(
            pending->sb, new_file_block, new_object);
        if (ret)
            goto finish;
    }

    if (file_repoint) {
        memcpy(change.object_id, ii->object_id, 16);
        change.object_block = new_file_block;
        change.object_type = INFILFS_OBJECT_FILE;
        ret = infilfs_native_index_update(pending, &change, 1);
        if (ret)
            goto finish;
    }

    pending->tx.next_sb.generation = cpu_to_le64(pending->tx.generation);

finish:
    if (pending) {
        int finish_ret = infilfs_ns_finish(pending, ret);

        pending = NULL;
        if (!ret)
            ret = finish_ret;
    }
    if (ret)
        goto out;

    ii->object_block = new_file_block;
    infilfs_refresh_inode_blocks_after_commit(inode);
    request->moved_bytes =
        (preserve_boundaries ? segment_physical_blocks : segment_blocks) <<
        INFILFS_DISK_BLOCK_SHIFT;

    ret = infilfs_native_pending_flush_sb(inode->i_sb);
    if (ret)
        goto out;

    ret = infilfs_fragmentation_metrics(inode, &after);
    if (ret)
        goto out;
    request->after_data_extents = after.data_extents;
    request->after_fragmentation_milli = after.fragmentation_milli;

out:
    kvfree(new_extents);
    kvfree(old_extents);
    kfree(new_object);
    kfree(old_object);
    return ret;
}

static long infilfs_file_ioctl(
    struct file *file, unsigned int command, unsigned long argument)
{
    struct inode *inode = file_inode(file);

    if (command == INFILFS_IOC_GET_FRAGMENTATION) {
        struct infilfs_fragmentation_metrics metrics;
        int ret = infilfs_fragmentation_metrics(inode, &metrics);

        if (ret)
            return ret;
        if (copy_to_user((void __user *)argument, &metrics, sizeof(metrics)))
            return -EFAULT;
        return 0;
    }

    if (command == INFILFS_IOC_DEFRAG_FILE) {
        struct infilfs_defrag_request request;
        int ret;

        if (!(file->f_mode & FMODE_WRITE))
            return -EBADF;
        if (copy_from_user(&request, (void __user *)argument,
                           sizeof(request)))
            return -EFAULT;
        memset(request.reserved, 0, sizeof(request.reserved));
        ret = infilfs_defrag_one(inode, &request);
        if (ret)
            return ret;
        if (copy_to_user((void __user *)argument, &request, sizeof(request)))
            return -EFAULT;
        return 0;
    }

    return -ENOTTY;
}
