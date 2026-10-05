static int infilfs_reflink_materialize_boundaries(
    struct infilfs_native_pending *pending, struct inode *inode,
    const struct infilfs_file_payload_disk *file,
    struct infilfs_extent_disk **extents_inout, u32 *extent_count_inout,
    u64 logical_blocks, u64 range_start, u64 range_blocks,
    bool *changed_out)
{
    u64 range_end;
    int ret;

    if (changed_out)
        *changed_out = false;
    if (!range_blocks || range_start > U64_MAX - range_blocks)
        return -EINVAL;
    range_end = range_start + range_blocks;

    if (range_start < logical_blocks) {
        u64 physical = 0;
        u64 extent_start = 0;
        u64 extent_end;
        u32 flags = INFILFS_EXTENT_HOLE;
        u32 extent_blocks = 0;

        ret = infilfs_native_map_extent_detail(
            *extents_inout, *extent_count_inout, range_start,
            &physical, &flags, &extent_start, &extent_blocks);
        if (ret)
            return ret;
        if (extent_start > U64_MAX - extent_blocks)
            return -EFSCORRUPTED;
        extent_end = extent_start + extent_blocks;
        if (infilfs_extent_is_compressed(flags) &&
            range_start > extent_start && range_start < extent_end) {
            ret = infilfs_native_materialize_compressed_at_locked(
                pending, inode, file, extents_inout, extent_count_inout,
                logical_blocks, range_start);
            if (ret)
                return ret;
            if (changed_out)
                *changed_out = true;
        }
    }

    if (range_end && range_end < logical_blocks) {
        u64 boundary = range_end - 1u;
        u64 physical = 0;
        u64 extent_start = 0;
        u64 extent_end;
        u32 flags = INFILFS_EXTENT_HOLE;
        u32 extent_blocks = 0;

        ret = infilfs_native_map_extent_detail(
            *extents_inout, *extent_count_inout, boundary,
            &physical, &flags, &extent_start, &extent_blocks);
        if (ret)
            return ret;
        if (extent_start > U64_MAX - extent_blocks)
            return -EFSCORRUPTED;
        extent_end = extent_start + extent_blocks;
        if (infilfs_extent_is_compressed(flags) &&
            range_end > extent_start && range_end < extent_end) {
            ret = infilfs_native_materialize_compressed_at_locked(
                pending, inode, file, extents_inout, extent_count_inout,
                logical_blocks, boundary);
            if (ret)
                return ret;
            if (changed_out)
                *changed_out = true;
        }
    }

    return 0;
}

static int infilfs_reflink_extract_range(
    const struct infilfs_extent_disk *source, u32 source_count,
    u64 source_start, u64 block_count, u64 destination_start,
    struct infilfs_extent_disk **range_out, u32 *range_count_out)
{
    struct infilfs_extent_disk *range;
    u32 range_count = 0;
    u64 source_end;
    u32 i;
    int ret;

    if (!block_count || source_start > U64_MAX - block_count)
        return -EINVAL;
    source_end = source_start + block_count;
    range = kvmalloc_array(source_count + 2u, sizeof(*range), GFP_NOFS);
    if (!range)
        return -ENOMEM;

    for (i = 0; i < source_count; ++i) {
        u64 start = le64_to_cpu(source[i].logical_block);
        u64 blocks = le32_to_cpu(source[i].block_count);
        u64 end;
        u64 first;
        u64 last;
        u64 physical;
        u32 flags = le32_to_cpu(source[i].flags);

        if (!blocks || start > U64_MAX - blocks) {
            ret = -EFSCORRUPTED;
            goto fail;
        }
        end = start + blocks;
        if (end <= source_start || start >= source_end)
            continue;
        first = max_t(u64, start, source_start);
        last = min_t(u64, end, source_end);

        /*
         * Compressed extents are indivisible codec streams. Sharing a proper
         * subset would make the extent's stored-byte/codec contract ambiguous,
         * so range clone accepts only complete compressed streams.
         */
        if (infilfs_extent_is_compressed(flags) &&
            (first != start || last != end)) {
            ret = -EINVAL;
            goto fail;
        }

        physical = le64_to_cpu(source[i].physical_block);
        if (infilfs_extent_kind(flags) == INFILFS_EXTENT_NORMAL &&
            !infilfs_extent_is_compressed(flags))
            physical += first - start;
        else if (infilfs_extent_kind(flags) == INFILFS_EXTENT_HOLE)
            physical = 0;

        ret = infilfs_native_extent_append(
            range, source_count + 2u, &range_count,
            destination_start + first - source_start, physical,
            (u32)(last - first), flags);
        if (ret)
            goto fail;
    }

    if (!range_count) {
        ret = -EFSCORRUPTED;
        goto fail;
    }
    *range_out = range;
    *range_count_out = range_count;
    return 0;
fail:
    kvfree(range);
    return ret;
}

static int infilfs_reflink_splice_range(
    const struct infilfs_extent_disk *destination, u32 destination_count,
    u64 destination_blocks, u64 replace_start, u64 replace_count,
    const struct infilfs_extent_disk *replacement, u32 replacement_count,
    struct infilfs_extent_disk **result_out, u32 *result_count_out)
{
    struct infilfs_extent_disk *result;
    u32 capacity = destination_count + replacement_count + 4u;
    u32 result_count = 0;
    u64 replace_end;
    u32 i;
    int ret;

    if (!replace_count || replace_start > U64_MAX - replace_count)
        return -EINVAL;
    replace_end = replace_start + replace_count;
    result = kvmalloc_array(capacity, sizeof(*result), GFP_NOFS);
    if (!result)
        return -ENOMEM;

    for (i = 0; i < destination_count; ++i) {
        u64 start = le64_to_cpu(destination[i].logical_block);
        u64 blocks = le32_to_cpu(destination[i].block_count);
        u64 end = start + blocks;
        u64 keep_end;
        u64 physical = le64_to_cpu(destination[i].physical_block);
        u32 flags = le32_to_cpu(destination[i].flags);

        if (start >= replace_start)
            break;
        keep_end = min_t(u64, end, replace_start);
        if (infilfs_extent_is_compressed(flags) && keep_end != end) {
            ret = -EINVAL;
            goto fail;
        }
        ret = infilfs_native_extent_append(
            result, capacity, &result_count, start,
            infilfs_extent_kind(flags) == INFILFS_EXTENT_NORMAL ?
                physical : 0,
            (u32)(keep_end - start), flags);
        if (ret)
            goto fail;
    }

    if (destination_blocks < replace_start) {
        u64 gap = replace_start - destination_blocks;

        if (gap > U32_MAX) {
            ret = -EFBIG;
            goto fail;
        }
        ret = infilfs_native_extent_append(
            result, capacity, &result_count, destination_blocks, 0,
            (u32)gap, INFILFS_EXTENT_HOLE);
        if (ret)
            goto fail;
    }

    for (i = 0; i < replacement_count; ++i) {
        ret = infilfs_native_extent_append(
            result, capacity, &result_count,
            le64_to_cpu(replacement[i].logical_block),
            le64_to_cpu(replacement[i].physical_block),
            le32_to_cpu(replacement[i].block_count),
            le32_to_cpu(replacement[i].flags));
        if (ret)
            goto fail;
    }

    for (i = 0; i < destination_count; ++i) {
        u64 start = le64_to_cpu(destination[i].logical_block);
        u64 blocks = le32_to_cpu(destination[i].block_count);
        u64 end = start + blocks;
        u64 keep_start;
        u64 physical = le64_to_cpu(destination[i].physical_block);
        u32 flags = le32_to_cpu(destination[i].flags);

        if (end <= replace_end)
            continue;
        keep_start = max_t(u64, start, replace_end);
        if (infilfs_extent_is_compressed(flags) && keep_start != start) {
            ret = -EINVAL;
            goto fail;
        }
        if (infilfs_extent_kind(flags) == INFILFS_EXTENT_NORMAL &&
            !infilfs_extent_is_compressed(flags))
            physical += keep_start - start;
        else if (infilfs_extent_kind(flags) == INFILFS_EXTENT_HOLE)
            physical = 0;
        ret = infilfs_native_extent_append(
            result, capacity, &result_count, keep_start, physical,
            (u32)(end - keep_start), flags);
        if (ret)
            goto fail;
    }

    *result_out = result;
    *result_count_out = result_count;
    return 0;
fail:
    kvfree(result);
    return ret;
}
