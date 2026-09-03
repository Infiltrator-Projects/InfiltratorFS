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
    u64 physical_runs = 0;
    u64 previous_physical_end = 0;
    u64 largest = 0;
    bool have_previous = false;
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
            u64 source = le64_to_cpu(extents[i].physical_block);

            allocated += physical_blocks;
            data_extents++;
            largest = max_t(u64, largest, physical_blocks);
            if (!have_previous || source != previous_physical_end)
                physical_runs++;
            previous_physical_end = source + physical_blocks;
            have_previous = true;
        } else if (infilfs_extent_kind(flags) == INFILFS_EXTENT_HOLE) {
            hole_extents++;
            have_previous = false;
        } else {
            have_previous = false;
        }
    }

    metrics->allocated_blocks = allocated;
    metrics->data_extents = data_extents;
    metrics->hole_extents = hole_extents;
    metrics->physical_runs = physical_runs;
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


struct infilfs_reflink_checksum_clone {
    u8 source_id[16];
    u8 clone_id[16];
    u64 source_block;
};

static int infilfs_reflink_clone_checksum_chain(
    struct infilfs_native_pending *pending,
    const struct infilfs_file_payload_disk *source_file,
    const u8 source_owner[16], const u8 clone_owner[16],
    struct infilfs_file_payload_disk *clone_file)
{
    struct infilfs_reflink_checksum_clone *nodes = NULL;
    struct infilfs_native_index_change *changes = NULL;
    u8 current[16];
    u8 *object = NULL;
    u32 count = 0;
    u32 capacity = 0;
    unsigned int guard;
    int ret = 0;
    u32 i;

    memset(clone_file->checksum_head_id, 0,
           sizeof(clone_file->checksum_head_id));
    memcpy(current, source_file->checksum_head_id, sizeof(current));
    if (!memchr_inv(current, 0, sizeof(current)))
        return 0;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;

    for (guard = 0; memchr_inv(current, 0, sizeof(current)) &&
         guard < 1048576u; ++guard) {
        struct infilfs_native_checksum_payload_disk *payload;
        struct infilfs_reflink_checksum_clone *grown;
        u8 next[16];
        u64 block;
        u16 type;

        ret = infilfs_index_lookup(pending->sb, current, &block, &type);
        if (ret)
            goto out;
        if (type != INFILFS_OBJECT_CHECKSUM) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        ret = infilfs_native_checksum_decode(
            pending->sb, source_owner, current, block, object, &payload);
        if (ret)
            goto out;
        memcpy(next, payload->next_object_id, sizeof(next));

        if (count == capacity) {
            u32 new_capacity = capacity ? capacity * 2u : 16u;

            grown = kvmalloc_array(
                new_capacity, sizeof(*grown), GFP_NOFS);
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
        memcpy(nodes[count].source_id, current, 16);
        nodes[count].source_block = block;
        ret = infilfs_native_random_id(nodes[count].clone_id);
        if (ret)
            goto out;
        count++;
        memcpy(current, next, sizeof(current));
    }
    if (memchr_inv(current, 0, sizeof(current))) {
        ret = -ELOOP;
        goto out;
    }
    if (!count) {
        ret = -EFSCORRUPTED;
        goto out;
    }

    changes = kvmalloc_array(count, sizeof(*changes), GFP_NOFS);
    if (!changes) {
        ret = -ENOMEM;
        goto out;
    }
    memset(changes, 0, (size_t)count * sizeof(*changes));

    for (i = 0; i < count; ++i) {
        struct infilfs_object_header_disk *header;
        struct infilfs_native_checksum_payload_disk *payload;
        u64 clone_block;

        ret = infilfs_native_checksum_decode(
            pending->sb, source_owner, nodes[i].source_id,
            nodes[i].source_block, object, &payload);
        if (ret)
            goto out;

        header = (struct infilfs_object_header_disk *)object;
        header->generation = cpu_to_le64(pending->tx.generation);
        memcpy(header->object_id, nodes[i].clone_id, 16);
        memcpy(header->parent_id, clone_owner, 16);
        memcpy(payload->owner_object_id, clone_owner, 16);
        memset(payload->next_object_id, 0,
               sizeof(payload->next_object_id));
        if (i + 1u < count)
            memcpy(payload->next_object_id, nodes[i + 1u].clone_id, 16);

        ret = infilfs_rw_finalize_object(object);
        if (ret)
            goto out;
        ret = infilfs_rw_tx_alloc(&pending->tx, 1, &clone_block);
        if (ret)
            goto out;
        ret = infilfs_native_stage_block(
            pending->sb, clone_block, object);
        if (ret)
            goto out;

        memcpy(changes[i].object_id, nodes[i].clone_id, 16);
        changes[i].object_block = clone_block;
        changes[i].object_type = INFILFS_OBJECT_CHECKSUM;
        changes[i].add = true;
    }

    ret = infilfs_native_index_update(pending, changes, count);
    if (!ret)
        memcpy(clone_file->checksum_head_id, nodes[0].clone_id, 16);
out:
    kfree(object);
    kvfree(changes);
    kvfree(nodes);
    return ret;
}

static int infilfs_native_reflink_full(
    struct file *source_file, struct file *destination_file)
{
    struct inode *source_inode = file_inode(source_file);
    struct inode *destination_inode = file_inode(destination_file);
    struct infilfs_inode_info *source_ii = INFILFS_I(source_inode);
    struct infilfs_inode_info *destination_ii = INFILFS_I(destination_inode);
    struct infilfs_native_pending *pending = NULL;
    struct infilfs_extent_disk *source_extents = NULL;
    struct infilfs_extent_disk *destination_extents = NULL;
    struct infilfs_file_payload_disk working_file;
    struct infilfs_object_header_disk *source_header;
    struct infilfs_object_header_disk *destination_header;
    struct infilfs_file_payload_disk *source_disk_file;
    struct infilfs_file_payload_disk *destination_disk_file;
    struct infilfs_native_index_change file_change = {0};
    u8 *source_object = NULL;
    u8 *destination_object = NULL;
    u8 *new_object = NULL;
    u8 *inline_data = NULL;
    u32 source_count = 0;
    u32 destination_count = 0;
    u64 source_blocks = 0;
    u64 destination_blocks = 0;
    u64 source_size;
    u64 new_file_block = 0;
    bool source_inline = false;
    bool destination_inline = false;
    bool file_repoint = false;
    int ret;

    if (!source_ii || !destination_ii ||
        source_inode->i_sb != destination_inode->i_sb)
        return -EXDEV;
    if (source_inode == destination_inode)
        return -EINVAL;
    if (!S_ISREG(source_inode->i_mode) ||
        !S_ISREG(destination_inode->i_mode))
        return -EINVAL;
    if ((le64_to_cpu(INFILFS_SB(destination_inode->i_sb)->disk.incompat_flags) &
         INFILFS_INCOMPAT_SHARED_EXTENTS) == 0)
        return -EOPNOTSUPP;
    if (!(destination_file->f_mode & FMODE_WRITE))
        return -EBADF;
    if (sb_rdonly(destination_inode->i_sb))
        return -EROFS;

    ret = filemap_write_and_wait(source_inode->i_mapping);
    if (ret)
        return ret;
    ret = filemap_write_and_wait(destination_inode->i_mapping);
    if (ret)
        return ret;

    source_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    destination_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    new_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!source_object || !destination_object || !new_object) {
        ret = -ENOMEM;
        goto out;
    }

    ret = infilfs_ns_begin(destination_inode->i_sb, &pending);
    if (ret)
        goto out;

    ret = infilfs_native_collect_extents(
        pending, source_inode, source_object, &source_extents,
        &source_count, &source_blocks, &source_inline);
    if (ret)
        goto finish;
    ret = infilfs_native_collect_extents(
        pending, destination_inode, destination_object, &destination_extents,
        &destination_count, &destination_blocks, &destination_inline);
    if (ret)
        goto finish;

    destination_header =
        (struct infilfs_object_header_disk *)destination_object;
    destination_disk_file =
        (struct infilfs_file_payload_disk *)(destination_header + 1);
    if (le64_to_cpu(destination_disk_file->attributes.logical_size) != 0 ||
        destination_count != 0 || destination_blocks != 0) {
        ret = -EOPNOTSUPP;
        goto finish;
    }

    source_header = (struct infilfs_object_header_disk *)source_object;
    source_disk_file =
        (struct infilfs_file_payload_disk *)(source_header + 1);
    source_size = le64_to_cpu(source_disk_file->attributes.logical_size);

    if (source_inline) {
        struct infilfs_object_header_disk *new_header;
        struct infilfs_file_payload_disk *new_file;
        struct infilfs_data_checksum_disk *digest;
        size_t loaded_size = 0;

        if (source_size > INFILFS_INLINE_DATA_MAX) {
            ret = -EFSCORRUPTED;
            goto finish;
        }
        inline_data = kzalloc(INFILFS_INLINE_DATA_MAX, GFP_NOFS);
        if (!inline_data) {
            ret = -ENOMEM;
            goto finish;
        }
        if (source_size) {
            ret = infilfs_rw_load_inline(
                source_inode, inline_data, &loaded_size);
            if (ret)
                goto finish;
            if (loaded_size != source_size) {
                ret = -EFSCORRUPTED;
                goto finish;
            }
        }

        memset(new_object, 0, INFILFS_DISK_BLOCK_SIZE);
        memcpy(new_object, destination_object,
               sizeof(*destination_header) + sizeof(*destination_disk_file));
        new_header = (struct infilfs_object_header_disk *)new_object;
        new_file = (struct infilfs_file_payload_disk *)(new_header + 1);
        new_header->generation = cpu_to_le64(pending->tx.generation);
        new_header->object_version =
            cpu_to_le16(INFILFS_OBJECT_VERSION_CLASSIC);
        new_file->attributes.logical_size = cpu_to_le64(source_size);
        new_file->attributes.modification_time_ns =
            cpu_to_le64(ktime_get_real_ns());
        new_file->attributes.change_time_ns =
            new_file->attributes.modification_time_ns;
        new_file->extent_count = 0;
        new_file->data_checksum_type = cpu_to_le32(INFILFS_CHECKSUM_SHA256);
        memset(new_file->checksum_head_id, 0,
               sizeof(new_file->checksum_head_id));
        if (source_size) {
            digest = (struct infilfs_data_checksum_disk *)(new_file + 1);
            ret = infilfs_rw_inline_digest(
                inline_data, (size_t)source_size, digest->bytes);
            if (ret)
                goto finish;
            memcpy(digest + 1, inline_data, (size_t)source_size);
            new_header->payload_size = cpu_to_le32(
                sizeof(*new_file) + sizeof(*digest) + (size_t)source_size);
        } else {
            new_header->payload_size = cpu_to_le32(sizeof(*new_file));
        }
        ret = infilfs_rw_finalize_object(new_object);
        if (ret)
            goto finish;
        ret = infilfs_native_store_private_or_cow(
            pending, destination_ii->object_block,
            new_object, &new_file_block);
        if (ret)
            goto finish;
        file_repoint = new_file_block != destination_ii->object_block;
    } else {
        memcpy(&working_file, destination_disk_file, sizeof(working_file));
        ret = infilfs_reflink_clone_checksum_chain(
            pending, source_disk_file, source_ii->object_id,
            destination_ii->object_id, &working_file);
        if (ret)
            goto finish;

        ret = infilfs_native_build_file_object(
            pending, destination_inode, destination_object, &working_file,
            source_extents, source_count, source_size,
            new_object, &new_file_block, &file_repoint);
        if (ret)
            goto finish;
    }

    if (file_repoint) {
        memcpy(file_change.object_id, destination_ii->object_id, 16);
        file_change.object_block = new_file_block;
        file_change.object_type = INFILFS_OBJECT_FILE;
        ret = infilfs_native_index_update(pending, &file_change, 1);
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
    if (!ret) {
        destination_ii->object_block = new_file_block;
        i_size_write(destination_inode, source_size);
        destination_inode->i_blocks =
            infilfs_native_extent_allocated_blocks(
                source_extents, source_count) *
            (INFILFS_DISK_BLOCK_SIZE >> 9);
        infilfs_native_checksum_cache_invalidate_sb(destination_inode->i_sb);
    }
out:
    kfree(inline_data);
    kvfree(destination_extents);
    kvfree(source_extents);
    kfree(new_object);
    kfree(destination_object);
    kfree(source_object);
    return ret;
}

static loff_t infilfs_file_remap_file_range(
    struct file *file_in, loff_t pos_in,
    struct file *file_out, loff_t pos_out,
    loff_t len, unsigned int remap_flags)
{
    struct inode *destination = file_inode(file_out);
    struct infilfs_quota_reservation quota = {0};
    loff_t source_size = i_size_read(file_inode(file_in));
    int ret;

    if (remap_flags || pos_in != 0 || pos_out != 0)
        return -EOPNOTSUPP;
    if (len == 0)
        len = source_size;
    if (len != source_size)
        return -EOPNOTSUPP;
    if (i_size_read(destination) != 0)
        return -EINVAL;

    ret = infilfs_quota_reserve_inode(
        destination, (u64)source_size, 0, &quota);
    if (ret)
        return ret;
    ret = infilfs_native_reflink_full(file_in, file_out);
    if (ret) {
        infilfs_quota_reservation_abort(&quota);
        return ret;
    }
    infilfs_quota_reservation_finish(
        &quota, (u64)source_size, 0);
    return len;
}

static long infilfs_file_ioctl(
    struct file *file, unsigned int command, unsigned long argument)
{
    struct inode *inode = file_inode(file);

    if (command == FICLONE) {
        struct file *source = fget((unsigned int)argument);
        int ret;

        if (!source)
            return -EBADF;
        ret = infilfs_native_reflink_full(source, file);
        fput(source);
        return ret;
    }

    if (command == INFILFS_IOC_SET_QUOTA ||
        command == INFILFS_IOC_GET_QUOTA) {
        struct infilfs_quota_request request;
        int ret;

        if (!capable(CAP_SYS_ADMIN))
            return -EPERM;
        if (copy_from_user(&request, (void __user *)argument, sizeof(request)))
            return -EFAULT;
        ret = command == INFILFS_IOC_SET_QUOTA ?
            infilfs_quota_set_rule(inode->i_sb, &request) :
            infilfs_quota_get_rule(inode->i_sb, &request);
        if (ret)
            return ret;
        if (copy_to_user((void __user *)argument, &request, sizeof(request)))
            return -EFAULT;
        return 0;
    }

    if (command == INFILFS_IOC_SET_PROJECT ||
        command == INFILFS_IOC_GET_PROJECT) {
        struct infilfs_project_request request;
        int ret;

        if (command == INFILFS_IOC_SET_PROJECT && !capable(CAP_SYS_ADMIN))
            return -EPERM;
        if (copy_from_user(&request, (void __user *)argument, sizeof(request)))
            return -EFAULT;
        ret = command == INFILFS_IOC_SET_PROJECT ?
            infilfs_quota_set_project(inode, &request) :
            infilfs_quota_get_project(inode, &request);
        if (ret)
            return ret;
        if (copy_to_user((void __user *)argument, &request, sizeof(request)))
            return -EFAULT;
        return 0;
    }

    if (command == INFILFS_IOC_RESIZE_VOLUME) {
        struct infilfs_resize_request request;
        int ret;

        if (!capable(CAP_SYS_ADMIN))
            return -EPERM;
        if (copy_from_user(&request, (void __user *)argument,
                           sizeof(request)))
            return -EFAULT;
        if (request.reserved0 || memchr_inv(request.reserved, 0,
                                            sizeof(request.reserved)))
            return -EINVAL;
        ret = infilfs_native_resize_volume(inode->i_sb, &request);
        if (ret)
            return ret;
        if (copy_to_user((void __user *)argument, &request, sizeof(request)))
            return -EFAULT;
        return 0;
    }

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
