static loff_t infilfs_file_remap_file_range(
    struct file *file_in, loff_t pos_in,
    struct file *file_out, loff_t pos_out,
    loff_t len, unsigned int remap_flags)
{
    struct inode *source = file_inode(file_in);
    struct inode *destination = file_inode(file_out);
    struct infilfs_quota_reservation quota = {0};
    loff_t source_size = i_size_read(source);
    u64 old_destination_size;
    u64 new_destination_size;
    u64 logical_growth;
    int ret;

    if (remap_flags)
        return -EOPNOTSUPP;
    if (pos_in < 0 || pos_out < 0)
        return -EINVAL;
    if (len == 0) {
        if (pos_in > source_size)
            return -EINVAL;
        len = source_size - pos_in;
    }
    if (len <= 0 || pos_in > source_size ||
        len > source_size - pos_in)
        return -EINVAL;

    old_destination_size = (u64)i_size_read(destination);
    if ((u64)pos_out > U64_MAX - (u64)len)
        return -EOVERFLOW;
    new_destination_size =
        max_t(u64, old_destination_size, (u64)pos_out + (u64)len);
    logical_growth = new_destination_size - old_destination_size;

    ret = infilfs_quota_reserve_inode(
        destination, logical_growth, 0, &quota);
    if (ret)
        return ret;

    /*
     * Preserve the compact inline whole-file clone fast path. Every other
     * request uses the extent-range implementation, including non-zero source
     * and destination offsets and replacement inside an existing file.
     */
    if (pos_in == 0 && pos_out == 0 &&
        len == source_size && old_destination_size == 0)
        ret = infilfs_native_reflink_full(file_in, file_out);
    else
        ret = infilfs_native_reflink_range(
            file_in, pos_in, file_out, pos_out, len);
    if (ret) {
        infilfs_quota_reservation_abort(&quota);
        return ret;
    }
    infilfs_quota_reservation_finish(&quota, logical_growth, 0);
    return len;
}

static long infilfs_file_ioctl(
    struct file *file, unsigned int command, unsigned long argument)
{
    struct inode *inode = file_inode(file);

    if (command == FICLONE) {
        struct file *source = fget((unsigned int)argument);
        loff_t ret;

        if (!source)
            return -EBADF;
        ret = infilfs_file_remap_file_range(
            source, 0, file, 0, 0, 0);
        fput(source);
        return ret < 0 ? (long)ret : 0;
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
