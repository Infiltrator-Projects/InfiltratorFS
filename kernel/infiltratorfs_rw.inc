// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Native Linux RW composition.
 *
 * Keep the already-qualified inline transaction engine intact as the legacy
 * implementation, then layer the extent-backed/deferred native data path over
 * the VFS entry points that need it.  This keeps the bring-up code reviewable
 * while native parity is expanded operation by operation.
 */

#include <linux/statfs.h>

#define infilfs_rw_tx_begin infilfs_rw_tx_begin_legacy
#define infilfs_file_write_iter infilfs_file_write_iter_legacy
#define infilfs_file_fsync infilfs_file_fsync_legacy
#define infilfs_rw_create infilfs_rw_create_legacy
#define infilfs_rw_mkdir infilfs_rw_mkdir_legacy
#define infilfs_rw_setattr infilfs_rw_setattr_legacy
#define infilfs_rw_mount_destroy infilfs_rw_mount_destroy_legacy
#include "infiltratorfs_rw_legacy.inc"
#undef infilfs_rw_tx_begin
#undef infilfs_file_write_iter
#undef infilfs_file_fsync
#undef infilfs_rw_create
#undef infilfs_rw_mkdir
#undef infilfs_rw_setattr
#undef infilfs_rw_mount_destroy

/* A deferred native transaction must be able to read back the transaction-
 * private metadata it has just staged.  The generic read validators reject
 * object/page generations newer than the live superblock generation, so make
 * the working generation visible to this mount as soon as the deferred
 * transaction begins.  No checkpoint is published here: the on-disk committed
 * generation remains unchanged until infilfs_rw_tx_commit(). */
static int infilfs_rw_tx_begin(struct super_block *sb,
                               struct infilfs_rw_tx *tx)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    int ret = infilfs_rw_tx_begin_legacy(sb, tx);

    if (ret)
        return ret;
    tx->next_sb.generation = cpu_to_le64(tx->generation);
    sbi->disk.generation = cpu_to_le64(tx->generation);
    return 0;
}

#include "infiltratorfs_rw_data.inc"

/*
 * Native VFS metadata bridge.
 *
 * InfiltratorFS has always stored all four timestamps, including birth time,
 * in its portable on-disk attributes.  The first native adapter exposed only
 * inode type/size/ownership, which made statx(2) lose STATX_BTIME and caused
 * file managers such as Nautilus to display no creation date.  Report the
 * authoritative on-disk timestamps through ->getattr rather than inventing
 * adapter-local metadata.
 */
static const struct infilfs_attributes_disk *
infilfs_vfs_attributes(const u8 object[INFILFS_DISK_BLOCK_SIZE])
{
    const struct infilfs_object_header_disk *header =
        (const struct infilfs_object_header_disk *)object;

    switch (le16_to_cpu(header->object_type)) {
    case INFILFS_OBJECT_DIRECTORY:
        return &((const struct infilfs_directory_payload_disk *)(header + 1))
                    ->attributes;
    case INFILFS_OBJECT_FILE:
        return &((const struct infilfs_file_payload_disk *)(header + 1))
                    ->attributes;
    case INFILFS_OBJECT_SYMLINK:
        return &((const struct infilfs_symlink_payload_disk *)(header + 1))
                    ->attributes;
    default:
        return NULL;
    }
}

static struct timespec64 infilfs_vfs_decode_time(__le64 encoded)
{
    return ns_to_timespec64((s64)le64_to_cpu(encoded));
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_getattr(struct mnt_idmap *idmap, const struct path *path,
                           struct kstat *stat, u32 request_mask,
                           unsigned int query_flags)
#else
static int infilfs_getattr(struct user_namespace *idmap,
                           const struct path *path, struct kstat *stat,
                           u32 request_mask, unsigned int query_flags)
#endif
{
    struct inode *inode = d_inode(path->dentry);
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    const struct infilfs_attributes_disk *attributes;
    u8 *object;
    int ret;

    (void)query_flags;
    if (!ii)
        return -EIO;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!object)
        return -ENOMEM;
    ret = infilfs_read_object(inode->i_sb, ii->object_block,
                              ii->object_type, ii->object_id, object);
    if (ret)
        goto out;
    attributes = infilfs_vfs_attributes(object);
    if (!attributes) {
        ret = -EOPNOTSUPP;
        goto out;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    generic_fillattr(idmap, request_mask, inode, stat);
#else
    generic_fillattr(idmap, inode, stat);
#endif
    stat->atime = infilfs_vfs_decode_time(attributes->access_time_ns);
    stat->mtime = infilfs_vfs_decode_time(attributes->modification_time_ns);
    stat->ctime = infilfs_vfs_decode_time(attributes->change_time_ns);
    stat->btime = infilfs_vfs_decode_time(attributes->birth_time_ns);
    stat->result_mask |= STATX_ATIME | STATX_MTIME | STATX_CTIME | STATX_BTIME;
    ret = 0;
out:
    kfree(object);
    return ret;
}

/*
 * Report real filesystem capacity to statfs(2).  simple_statfs() intentionally
 * leaves block accounting at zero, which made desktop file managers lose the
 * used/free-space pie chart after the switch from FUSE to the native VFS.
 */
static int infilfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
    struct super_block *sb = dentry->d_sb;
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    u64 total;
    u64 free_blocks;

    if (!sbi)
        return -EIO;
    total = le64_to_cpu(sbi->disk.total_blocks);
    free_blocks = le64_to_cpu(sbi->disk.free_blocks);
    if (!total || free_blocks > total)
        return -EFSCORRUPTED;

    memset(buf, 0, sizeof(*buf));
    buf->f_type = INFILTRATORFS_MAGIC;
    buf->f_bsize = INFILFS_DISK_BLOCK_SIZE;
    buf->f_frsize = INFILFS_DISK_BLOCK_SIZE;
    buf->f_blocks = total;
    buf->f_bfree = free_blocks;
    buf->f_bavail = free_blocks;
    /* Object count has no fixed inode table; these are conservative upper
     * bounds used only for generic VFS reporting. */
    buf->f_files = total;
    buf->f_ffree = free_blocks;
    buf->f_namelen = INFILFS_NAME_MAX;
    return 0;
}

/*
 * infiltratorfs.c declares the operation tables immediately after including
 * this composition file.  Keep the table source compact while supplying the
 * native metadata callbacks here beside the rest of the RW adapter.  Each
 * macro expands one existing initializer value plus the additional designated
 * initializer; the function token in its own replacement list is deliberately
 * not recursively expanded by the C preprocessor.
 */
#define simple_statfs infilfs_statfs
#define infilfs_rw_mkdir infilfs_rw_mkdir, .getattr = infilfs_getattr
#define infilfs_rw_setattr infilfs_rw_setattr, .getattr = infilfs_getattr
#define infilfs_get_link infilfs_get_link, .getattr = infilfs_getattr
