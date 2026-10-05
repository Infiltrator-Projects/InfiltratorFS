#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
path = root / "kernel" / "infiltratorfs_defrag.inc"
text = path.read_text()

helper = r'''
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

'''

marker = "\n\nstatic int infilfs_reflink_extract_range("
if "static int infilfs_reflink_materialize_boundaries(" not in text:
    if marker not in text:
        raise SystemExit("reflink extract marker missing")
    text = text.replace(marker, "\n\n" + helper + "static int infilfs_reflink_extract_range(", 1)

start = text.index("static int infilfs_native_reflink_range(\n")
end = text.index("\nstatic loff_t infilfs_file_remap_file_range(", start)

replacement = r'''static int infilfs_native_reflink_range(
    struct file *source_file, loff_t pos_in,
    struct file *destination_file, loff_t pos_out, loff_t len)
{
    struct inode *source_inode = file_inode(source_file);
    struct inode *destination_inode = file_inode(destination_file);
    struct infilfs_inode_info *source_ii = INFILFS_I(source_inode);
    struct infilfs_inode_info *destination_ii = INFILFS_I(destination_inode);
    struct infilfs_native_pending *pending = NULL;
    struct infilfs_extent_disk *source_extents = NULL;
    struct infilfs_extent_disk *destination_extents = NULL;
    struct infilfs_extent_disk *clone_extents = NULL;
    struct infilfs_extent_disk *new_extents = NULL;
    struct infilfs_data_checksum_disk *digests = NULL;
    struct infilfs_native_index_change *changes = NULL;
    struct infilfs_file_payload_disk source_working_file;
    struct infilfs_file_payload_disk working_file;
    struct infilfs_object_header_disk *source_header;
    struct infilfs_object_header_disk *destination_header;
    struct infilfs_file_payload_disk *source_disk_file;
    struct infilfs_file_payload_disk *destination_disk_file;
    struct infilfs_native_read_checksum_cursor checksum_cursor = {0};
    struct infilfs_native_index_change source_change = {0};
    u8 *source_object = NULL;
    u8 *destination_object = NULL;
    u8 *new_object = NULL;
    u8 *checksum_object = NULL;
    u8 final_tail_id[16] = {0};
    u64 final_tail_block = 0;
    u64 final_tail_start = 0;
    u64 source_blocks = 0;
    u64 destination_blocks = 0;
    u64 source_size;
    u64 start_block;
    u64 destination_start;
    u64 block_count;
    u64 new_size;
    u64 source_new_file_block = 0;
    u64 new_file_block = 0;
    u64 allocated_blocks;
    u32 source_count = 0;
    u32 destination_count = 0;
    u32 clone_count = 0;
    u32 new_count = 0;
    u32 change_count = 0;
    u32 change_capacity;
    bool source_inline = false;
    bool destination_inline = false;
    bool source_materialized = false;
    bool source_file_repoint = false;
    bool file_repoint = false;
    u64 logical;
    int ret;

    if (!source_ii || !destination_ii ||
        source_inode->i_sb != destination_inode->i_sb)
        return -EXDEV;
    if (source_inode == destination_inode)
        return -EINVAL;
    if (!S_ISREG(source_inode->i_mode) ||
        !S_ISREG(destination_inode->i_mode))
        return -EINVAL;
    if (!(destination_file->f_mode & FMODE_WRITE))
        return -EBADF;
    if (sb_rdonly(destination_inode->i_sb))
        return -EROFS;
    if ((le64_to_cpu(INFILFS_SB(destination_inode->i_sb)->disk.incompat_flags) &
         INFILFS_INCOMPAT_SHARED_EXTENTS) == 0)
        return -EOPNOTSUPP;
    if (pos_in < 0 || pos_out < 0 || len <= 0 ||
        ((u64)pos_in & (INFILFS_DISK_BLOCK_SIZE - 1u)) ||
        ((u64)pos_out & (INFILFS_DISK_BLOCK_SIZE - 1u)))
        return -EINVAL;
    if ((u64)pos_in > (u64)i_size_read(source_inode) ||
        (u64)len > (u64)i_size_read(source_inode) - (u64)pos_in)
        return -EINVAL;

    /*
     * A non-block-aligned tail is shareable only when it is the source EOF and
     * the destination clone also ends at its EOF. Otherwise bytes outside the
     * requested range share the same physical block and must be preserved by a
     * read/modify/write rather than by remapping the complete block.
     */
    if (((u64)len & (INFILFS_DISK_BLOCK_SIZE - 1u)) != 0) {
        u64 source_end = (u64)pos_in + (u64)len;
        u64 destination_end = (u64)pos_out + (u64)len;

        if (source_end != (u64)i_size_read(source_inode) ||
            destination_end < (u64)i_size_read(destination_inode))
            return -EINVAL;
    }

    ret = filemap_write_and_wait(source_inode->i_mapping);
    if (ret)
        return ret;
    ret = filemap_write_and_wait(destination_inode->i_mapping);
    if (ret)
        return ret;

    source_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    destination_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    new_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    checksum_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!source_object || !destination_object ||
        !new_object || !checksum_object) {
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

    /* Inline data has no shareable physical extent. Whole-file clone already
     * handles it exactly; range clone begins once data has extent storage. */
    if (source_inline || destination_inline) {
        ret = -EOPNOTSUPP;
        goto finish;
    }

    source_header = (struct infilfs_object_header_disk *)source_object;
    source_disk_file =
        (struct infilfs_file_payload_disk *)(source_header + 1);
    destination_header =
        (struct infilfs_object_header_disk *)destination_object;
    destination_disk_file =
        (struct infilfs_file_payload_disk *)(destination_header + 1);
    source_size = le64_to_cpu(source_disk_file->attributes.logical_size);

    start_block = (u64)pos_in >> INFILFS_DISK_BLOCK_SHIFT;
    destination_start = (u64)pos_out >> INFILFS_DISK_BLOCK_SHIFT;
    block_count = DIV_ROUND_UP_ULL((u64)len, INFILFS_DISK_BLOCK_SIZE);
    if (!block_count || block_count > U32_MAX) {
        ret = -EFBIG;
        goto finish;
    }

    /*
     * A compressed extent is an indivisible codec stream. If either source or
     * destination range boundary cuts through one, materialize only that
     * boundary stream into ordinary blocks inside this transaction. The source
     * object is republished only when its physical representation changed;
     * logical bytes, checksums and timestamps remain identical.
     */
    ret = infilfs_reflink_materialize_boundaries(
        pending, source_inode, source_disk_file,
        &source_extents, &source_count, source_blocks,
        start_block, block_count, &source_materialized);
    if (ret)
        goto finish;
    ret = infilfs_reflink_materialize_boundaries(
        pending, destination_inode, destination_disk_file,
        &destination_extents, &destination_count, destination_blocks,
        destination_start, block_count, NULL);
    if (ret)
        goto finish;

    if (source_materialized) {
        memcpy(&source_working_file, source_disk_file,
               sizeof(source_working_file));
        ret = infilfs_native_build_file_object(
            pending, source_inode, source_object, &source_working_file,
            source_extents, source_count, source_size,
            new_object, &source_new_file_block, &source_file_repoint);
        if (ret)
            goto finish;
        if (source_file_repoint) {
            memcpy(source_change.object_id, source_ii->object_id, 16);
            source_change.object_block = source_new_file_block;
            source_change.object_type = INFILFS_OBJECT_FILE;
            ret = infilfs_native_index_update(pending, &source_change, 1);
            if (ret)
                goto finish;
        }
    }

    ret = infilfs_reflink_extract_range(
        source_extents, source_count, start_block, block_count,
        destination_start, &clone_extents, &clone_count);
    if (ret)
        goto finish;
    ret = infilfs_reflink_splice_range(
        destination_extents, destination_count, destination_blocks,
        destination_start, block_count, clone_extents, clone_count,
        &new_extents, &new_count);
    if (ret)
        goto finish;

    digests = kvmalloc_array(block_count, sizeof(*digests), GFP_NOFS);
    if (!digests) {
        ret = -ENOMEM;
        goto finish;
    }
    for (logical = 0; logical < block_count; ++logical) {
        ret = infilfs_native_read_expected_digest(
            source_inode->i_sb, source_ii->object_id,
            source_disk_file->checksum_head_id, start_block + logical,
            &checksum_cursor, checksum_object, &digests[logical]);
        if (ret)
            goto finish;
    }

    memcpy(&working_file, destination_disk_file, sizeof(working_file));

    change_capacity =
        (u32)min_t(u64, U32_MAX,
            8u + 2u * DIV_ROUND_UP_ULL(
                block_count, INFILFS_NATIVE_CHECKSUMS_PER_OBJECT));
    changes = kcalloc(change_capacity, sizeof(*changes), GFP_NOFS);
    if (!changes) {
        ret = -ENOMEM;
        goto finish;
    }
    ret = infilfs_native_checksum_set_range(
        pending, &working_file, destination_ii->object_id,
        destination_start, block_count, digests,
        changes, change_capacity, &change_count,
        final_tail_id, &final_tail_block, &final_tail_start);
    if (ret)
        goto finish;

    ret = infilfs_native_release_replaced_extents(
        pending, destination_ii->object_id,
        destination_extents, destination_count,
        destination_start, block_count);
    if (ret)
        goto finish;

    new_size = max_t(u64, le64_to_cpu(
        destination_disk_file->attributes.logical_size),
        (u64)pos_out + (u64)len);
    ret = infilfs_native_build_file_object(
        pending, destination_inode, destination_object, &working_file,
        new_extents, new_count, new_size,
        new_object, &new_file_block, &file_repoint);
    if (ret)
        goto finish;

    if (file_repoint) {
        if (change_count >= change_capacity) {
            ret = -ENOSPC;
            goto finish;
        }
        memcpy(changes[change_count].object_id,
               destination_ii->object_id, 16);
        changes[change_count].object_block = new_file_block;
        changes[change_count].object_type = INFILFS_OBJECT_FILE;
        change_count++;
    }
    ret = infilfs_native_index_update(pending, changes, change_count);
    if (ret)
        goto finish;

    if (pending->shared_range_index_valid) {
        int ownership_ret = infilfs_shared_ownership_add_owner(
            pending, clone_extents, clone_count);

        if (ownership_ret) {
            kvfree(pending->shared_ranges);
            pending->shared_ranges = NULL;
            pending->shared_range_count = 0;
            pending->shared_range_index_valid = false;
            if (ownership_ret != -ENOMEM && ownership_ret != -EOVERFLOW) {
                ret = ownership_ret;
                goto finish;
            }
        }
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
        if (source_materialized) {
            source_ii->object_block = source_new_file_block;
            source_inode->i_blocks =
                infilfs_native_extent_allocated_blocks(
                    source_extents, source_count) *
                (INFILFS_DISK_BLOCK_SIZE >> 9);
        }
        destination_ii->object_block = new_file_block;
        WRITE_ONCE(destination_ii->persisted_size, new_size);
        i_size_write(destination_inode, new_size);
        allocated_blocks =
            infilfs_native_extent_allocated_blocks(new_extents, new_count);
        destination_inode->i_blocks =
            allocated_blocks * (INFILFS_DISK_BLOCK_SIZE >> 9);
        infilfs_native_checksum_cache_store(
            destination_inode->i_sb, destination_ii->object_id,
            final_tail_id, final_tail_block, final_tail_start);
        infilfs_native_checksum_cache_invalidate_sb(destination_inode->i_sb);
    }
out:
    kfree(checksum_object);
    kfree(changes);
    kvfree(digests);
    kvfree(new_extents);
    kvfree(clone_extents);
    kvfree(destination_extents);
    kvfree(source_extents);
    kfree(new_object);
    kfree(destination_object);
    kfree(source_object);
    return ret;
}
'''

text = text[:start] + replacement + text[end:]
path.write_text(text)

policy = root / "tests" / "native-reflink-scaling-policy.sh"
p = policy.read_text()
needle = "grep -Fq 'infilfs_reflink_extract_range(' \"$reflink\" ||\n    fail 'range reflink does not extract source extent ranges'\n"
extra = needle + "grep -Fq 'infilfs_reflink_materialize_boundaries(' \"$reflink\" ||\n    fail 'range reflink does not materialize compressed boundary streams'\ngrep -Fq 'infilfs_native_materialize_compressed_at_locked(' \"$reflink\" ||\n    fail 'compressed range-reflink boundary materialization is disconnected'\n"
if "range reflink does not materialize compressed boundary streams" not in p:
    if needle not in p:
        raise SystemExit("reflink policy insertion marker missing")
    p = p.replace(needle, extra, 1)
    policy.write_text(p)

print("staged compressed-boundary range reflink fix")
