// SPDX-License-Identifier: GPL-3.0-or-later
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/uidgid.h>

#define INFILTRATORFS_NAME "infiltratorfs"
#define INFILTRATORFS_MAGIC 0x494e4653u
#define INFILTRATORFS_BLOCK_SIZE 4096u
#define INFILTRATORFS_BLOCK_SHIFT 12u
#define INFILTRATORFS_FORMAT_MAJOR 0u
#define INFILTRATORFS_FORMAT_MINOR 12u

static const unsigned char infilfs_disk_magic[8] = {
    'I', 'N', 'F', 'S', '2', '0', '2', '6'
};

/*
 * Only the stable prefix needed by the bootstrap reader is duplicated here.
 * The native driver must not depend on userspace libc headers. As the kernel
 * adapter grows, persistent layout declarations will move into a dedicated
 * kernel-safe shared format header rather than being redefined ad hoc.
 */
struct infilfs_superblock_prefix_disk {
    u8 magic[8];
    __le16 format_major;
    __le16 format_minor;
    __le16 header_size;
    __le16 block_shift;
    __le32 checksum_type;
    __le64 generation;
} __packed;

static const struct super_operations infilfs_super_operations = {
    .statfs = simple_statfs,
    .drop_inode = generic_delete_inode,
};

static struct inode *infilfs_make_root_inode(struct super_block *sb)
{
    struct inode *inode = new_inode(sb);

    if (!inode)
        return NULL;

    inode->i_ino = 1;
    inode->i_mode = S_IFDIR | 0555;
    inode->i_uid = GLOBAL_ROOT_UID;
    inode->i_gid = GLOBAL_ROOT_GID;
    set_nlink(inode, 2);
    inode->i_op = &simple_dir_inode_operations;
    inode->i_fop = &simple_dir_operations;
    return inode;
}

static int infilfs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct buffer_head *bh;
    struct infilfs_superblock_prefix_disk *disk;
    struct inode *root_inode;
    u16 major;
    u16 minor;
    u16 header_size;
    u16 block_shift;
    u64 generation;

    (void)data;
    (void)silent;

    /* Phase one is intentionally read-only. */
    if (!(sb->s_flags & SB_RDONLY))
        return -EROFS;

    if (!sb_set_blocksize(sb, INFILTRATORFS_BLOCK_SIZE))
        return -EINVAL;

    bh = sb_bread(sb, 0);
    if (!bh)
        return -EIO;

    disk = (struct infilfs_superblock_prefix_disk *)bh->b_data;
    if (memcmp(disk->magic, infilfs_disk_magic, sizeof(infilfs_disk_magic)) != 0) {
        brelse(bh);
        return -EINVAL;
    }

    major = le16_to_cpu(disk->format_major);
    minor = le16_to_cpu(disk->format_minor);
    header_size = le16_to_cpu(disk->header_size);
    block_shift = le16_to_cpu(disk->block_shift);
    generation = le64_to_cpu(disk->generation);

    if (major != INFILTRATORFS_FORMAT_MAJOR ||
        minor != INFILTRATORFS_FORMAT_MINOR ||
        block_shift != INFILTRATORFS_BLOCK_SHIFT ||
        header_size < sizeof(*disk) || header_size > INFILTRATORFS_BLOCK_SIZE ||
        generation == 0) {
        brelse(bh);
        return -EINVAL;
    }

    sb->s_magic = INFILTRATORFS_MAGIC;
    sb->s_maxbytes = MAX_LFS_FILESIZE;
    sb->s_time_gran = 1;
    sb->s_op = &infilfs_super_operations;

    root_inode = infilfs_make_root_inode(sb);
    if (!root_inode) {
        brelse(bh);
        return -ENOMEM;
    }

    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root) {
        brelse(bh);
        return -ENOMEM;
    }

    pr_info("InfiltratorFS: mounted Format %u.%u generation %llu read-only\n",
            major, minor, (unsigned long long)generation);
    brelse(bh);
    return 0;
}

static struct dentry *infilfs_mount(struct file_system_type *fs_type,
                                    int flags, const char *dev_name,
                                    void *data)
{
    return mount_bdev(fs_type, flags, dev_name, data, infilfs_fill_super);
}

static struct file_system_type infilfs_type = {
    .owner = THIS_MODULE,
    .name = INFILTRATORFS_NAME,
    .mount = infilfs_mount,
    .kill_sb = kill_block_super,
    .fs_flags = FS_REQUIRES_DEV,
};

static int __init infilfs_init(void)
{
    int status = register_filesystem(&infilfs_type);

    if (status == 0)
        pr_info("InfiltratorFS: native Linux VFS bootstrap registered\n");
    return status;
}

static void __exit infilfs_exit(void)
{
    unregister_filesystem(&infilfs_type);
    pr_info("InfiltratorFS: native Linux VFS bootstrap unloaded\n");
}

module_init(infilfs_init);
module_exit(infilfs_exit);

MODULE_DESCRIPTION("InfiltratorFS native Linux read-only VFS bootstrap");
MODULE_AUTHOR("The First Infiltrator");
MODULE_LICENSE("GPL");
MODULE_ALIAS_FS(INFILTRATORFS_NAME);
