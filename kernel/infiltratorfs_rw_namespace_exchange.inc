// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Cross-directory RENAME_EXCHANGE is kept in its own leaf implementation unit
 * so the namespace compositor stays below the maintainability ceiling without
 * weakening the single-translation-unit transaction helpers it relies on.
 */
static int infilfs_ns_rename_exchange_cross_dir(
    struct inode *old_dir, struct dentry *old_dentry,
    struct inode *new_dir, struct dentry *new_dentry)
{
    struct inode *source = d_inode(old_dentry);
    struct inode *destination = d_inode(new_dentry);
    struct infilfs_inode_info *source_ii = source ? INFILFS_I(source) : NULL;
    struct infilfs_inode_info *destination_ii =
        destination ? INFILFS_I(destination) : NULL;
    struct infilfs_inode_info *old_dir_ii = INFILFS_I(old_dir);
    struct infilfs_inode_info *new_dir_ii = INFILFS_I(new_dir);
    struct infilfs_native_pending *pending = NULL;
    struct infilfs_ns_index_change *changes = NULL;
    struct infilfs_quota_reservation source_new_quota = {0};
    struct infilfs_quota_reservation source_old_quota = {0};
    struct infilfs_quota_reservation destination_new_quota = {0};
    struct infilfs_quota_reservation destination_old_quota = {0};
    u32 change_count = 0, change_capacity = 0;
    u64 source_block = 0, destination_block = 0;
    u64 old_dir_block = 0, new_dir_block = 0;
    s64 old_dir_block_delta = 0, new_dir_block_delta = 0;
    int old_link_delta;
    int new_link_delta;
    int ret;

    if (!source || !destination || !source_ii || !destination_ii ||
        !old_dir_ii || !new_dir_ii)
        return -ENOENT;
    if (old_dir->i_sb != new_dir->i_sb)
        return -EXDEV;
    ret = infilfs_name_validate(old_dir->i_sb, &old_dentry->d_name);
    if (!ret)
        ret = infilfs_name_validate(new_dir->i_sb, &new_dentry->d_name);
    if (ret)
        return ret;
    if (source == destination)
        return 0;

    /*
     * Both directory moves must remain acyclic. Test each moving directory
     * against the parent it will acquire before starting the atomic mutation.
     */
    if (source_ii->object_type == INFILFS_OBJECT_DIRECTORY) {
        ret = infilfs_ns_is_ancestor(
            old_dir->i_sb, source_ii->object_id, new_dir_ii->object_id);
        if (ret < 0)
            return ret;
        if (ret)
            return -EINVAL;
    }
    if (destination_ii->object_type == INFILFS_OBJECT_DIRECTORY) {
        ret = infilfs_ns_is_ancestor(
            old_dir->i_sb, destination_ii->object_id, old_dir_ii->object_id);
        if (ret < 0)
            return ret;
        if (ret)
            return -EINVAL;
    }

    if (source_ii->object_type == INFILFS_OBJECT_FILE &&
        source->i_nlink > 1) {
        ret = infilfs_quota_hardlink_check(source, new_dir);
        if (ret)
            return ret;
    }
    if (destination_ii->object_type == INFILFS_OBJECT_FILE &&
        destination->i_nlink > 1) {
        ret = infilfs_quota_hardlink_check(destination, old_dir);
        if (ret)
            return ret;
    }

    mutex_lock(&INFILFS_SB(old_dir->i_sb)->quota_lock);
    ret = infilfs_ns_begin(old_dir->i_sb, &pending);
    if (ret)
        goto unlock;

    ret = infilfs_quota_prepare_reparent_locked(
        source, new_dir, &source_new_quota, &source_old_quota);
    if (!ret)
        ret = infilfs_quota_prepare_reparent_locked(
            destination, old_dir,
            &destination_new_quota, &destination_old_quota);
    if (ret)
        goto finish;

    ret = infilfs_ns_cow_object(
        pending, source,
        !(source_ii->object_type == INFILFS_OBJECT_FILE &&
          source->i_nlink > 1) ? new_dir_ii->object_id : NULL,
        0, false, &source_block, NULL);
    if (!ret)
        ret = infilfs_ns_cow_object(
            pending, destination,
            !(destination_ii->object_type == INFILFS_OBJECT_FILE &&
              destination->i_nlink > 1) ? old_dir_ii->object_id : NULL,
            0, false, &destination_block, NULL);
    if (ret)
        goto finish;

    old_link_delta =
        (destination_ii->object_type == INFILFS_OBJECT_DIRECTORY ? 1 : 0) -
        (source_ii->object_type == INFILFS_OBJECT_DIRECTORY ? 1 : 0);
    new_link_delta = -old_link_delta;

    ret = infilfs_ns_rebuild_directory_accounted(
        pending, old_dir, &old_dentry->d_name, NULL,
        &old_dentry->d_name, destination_ii->object_id,
        destination_ii->object_type, old_link_delta,
        &old_dir_block, &old_dir_block_delta);
    if (!ret)
        ret = infilfs_ns_rebuild_directory_accounted(
            pending, new_dir, &new_dentry->d_name, NULL,
            &new_dentry->d_name, source_ii->object_id,
            source_ii->object_type, new_link_delta,
            &new_dir_block, &new_dir_block_delta);
    if (!ret)
        ret = infilfs_ns_changes_append(
            &changes, &change_count, &change_capacity,
            source_ii->object_id, INFILFS_NS_REPOINT,
            source_block, source_ii->object_type);
    if (!ret)
        ret = infilfs_ns_changes_append(
            &changes, &change_count, &change_capacity,
            destination_ii->object_id, INFILFS_NS_REPOINT,
            destination_block, destination_ii->object_type);
    if (!ret)
        ret = infilfs_ns_changes_append(
            &changes, &change_count, &change_capacity,
            old_dir_ii->object_id, INFILFS_NS_REPOINT,
            old_dir_block, INFILFS_OBJECT_DIRECTORY);
    if (!ret)
        ret = infilfs_ns_changes_append(
            &changes, &change_count, &change_capacity,
            new_dir_ii->object_id, INFILFS_NS_REPOINT,
            new_dir_block, INFILFS_OBJECT_DIRECTORY);
    if (!ret)
        ret = infilfs_ns_rebuild_index(pending, changes, change_count);

finish:
    ret = infilfs_ns_finish(pending, ret);
    pending = NULL;

    if (ret) {
        if (source_new_quota.active)
            infilfs_quota_adjust_locked(
                &source_new_quota,
                source_new_quota.bytes, source_new_quota.objects);
        if (destination_new_quota.active)
            infilfs_quota_adjust_locked(
                &destination_new_quota,
                destination_new_quota.bytes,
                destination_new_quota.objects);
    } else {
        /*
         * Destination reservations become the committed usage. Remove the old
         * project usage only after the namespace transaction is durable.
         */
        if (source_old_quota.active)
            infilfs_quota_adjust_locked(
                &source_old_quota,
                source_old_quota.bytes, source_old_quota.objects);
        if (destination_old_quota.active)
            infilfs_quota_adjust_locked(
                &destination_old_quota,
                destination_old_quota.bytes,
                destination_old_quota.objects);
        infilfs_quota_project_cache_bump_locked(
            INFILFS_SB(old_dir->i_sb));
    }
    source_new_quota.active = false;
    source_old_quota.active = false;
    destination_new_quota.active = false;
    destination_old_quota.active = false;

unlock:
    mutex_unlock(&INFILFS_SB(old_dir->i_sb)->quota_lock);
    if (!ret) {
        source_ii->object_block = source_block;
        destination_ii->object_block = destination_block;
        old_dir_ii->object_block = old_dir_block;
        new_dir_ii->object_block = new_dir_block;
        infilfs_adjust_inode_blocks_after_commit(
            old_dir, old_dir_block_delta);
        infilfs_adjust_inode_blocks_after_commit(
            new_dir, new_dir_block_delta);
        while (old_link_delta < 0) {
            drop_nlink(old_dir);
            old_link_delta++;
        }
        while (old_link_delta > 0) {
            inc_nlink(old_dir);
            old_link_delta--;
        }
        while (new_link_delta < 0) {
            drop_nlink(new_dir);
            new_link_delta++;
        }
        while (new_link_delta > 0) {
            inc_nlink(new_dir);
            new_link_delta--;
        }
    }
    kvfree(changes);
    return ret;
}
