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
        infilfs_timestamp_encode_ns(&new_file->attributes.modification_time, ktime_get_real_ns());
        new_file->attributes.change_time =
            new_file->attributes.modification_time;
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

    /*
     * A non-inline reflink introduces one additional live owner for every
     * source physical range.  Maintain a valid ownership accelerator locally
     * instead of forcing the next reclamation to rebuild the entire map.
     */
    if (!source_inline && pending->shared_range_index_valid) {
        int ownership_ret = infilfs_shared_ownership_add_owner(
            pending, source_extents, source_count);

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
        destination_ii->object_block = new_file_block;
        WRITE_ONCE(destination_ii->persisted_size, source_size);
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


