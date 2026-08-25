// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Native Linux RW composition.
 *
 * Keep the already-qualified inline transaction engine intact as the legacy
 * implementation, then layer the extent-backed/deferred native data path,
 * transactional namespace mutations and the POSIX metadata bridge over the
 * VFS entry points that need them.
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

/*
 * A deferred native transaction must be able to read back the transaction-
 * private metadata it has just staged.  The generic read validators reject
 * object/page generations newer than the live superblock generation, so make
 * the working generation visible to this mount as soon as the deferred
 * transaction begins.  No checkpoint is published here: the on-disk committed
 * generation remains unchanged until infilfs_rw_tx_commit().
 */
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

/*
 * The data layer already wraps create/mkdir/setattr to flush a deferred data
 * transaction before entering the original synchronous metadata path.  Keep
 * those qualified wrappers available under private names so the final POSIX
 * layer can add ownership and general setattr semantics without duplicating
 * the data-path ordering rules.
 */
#define infilfs_rw_create infilfs_rw_create_data
#define infilfs_rw_mkdir infilfs_rw_mkdir_data
#define infilfs_rw_setattr infilfs_rw_setattr_data
#include "infiltratorfs_rw_data.inc"
#undef infilfs_rw_create
#undef infilfs_rw_mkdir
#undef infilfs_rw_setattr

/*
 * New native objects must carry the credentials of the task that created them.
 * The original bring-up code deliberately used uid/gid zero while write support
 * was root-only.  That made every file created by an ordinary desktop user
 * become root-owned once native mounts were exposed through Nautilus.
 */
static int infilfs_posix_uid_to_disk(kuid_t uid, u32 *value)
{
    uid_t raw = from_kuid(&init_user_ns, uid);

    if (raw == (uid_t)-1)
        return -EOVERFLOW;
    *value = (u32)raw;
    return 0;
}

static int infilfs_posix_gid_to_disk(kgid_t gid, u32 *value)
{
    gid_t raw = from_kgid(&init_user_ns, gid);

    if (raw == (gid_t)-1)
        return -EOVERFLOW;
    *value = (u32)raw;
    return 0;
}

static void infilfs_posix_current_ids(kuid_t *uid, kgid_t *gid)
{
    const struct cred *cred = get_current_cred();

    *uid = cred->fsuid;
    *gid = cred->fsgid;
    put_cred(cred);
}

static void infilfs_posix_fill_common_attributes(
    struct infilfs_attributes_disk *attributes,
    struct infilfs_posix_compat_disk *posix,
    umode_t mode, u64 links)
{
    u64 now = ktime_get_real_ns();
    kuid_t kernel_uid;
    kgid_t kernel_gid;
    u32 uid = 0;
    u32 gid = 0;

    /*
     * The disk format stores init-user-namespace numeric IDs.  The format
     * header intentionally removes Linux's current task macro, so obtain a
     * stable credential reference through the exported credential API rather
     * than depending on current_fsuid/current_fsgid macro expansion here.
     */
    infilfs_posix_current_ids(&kernel_uid, &kernel_gid);
    (void)infilfs_posix_uid_to_disk(kernel_uid, &uid);
    (void)infilfs_posix_gid_to_disk(kernel_gid, &gid);

    memset(attributes, 0, sizeof(*attributes));
    memset(posix, 0, sizeof(*posix));
    attributes->link_count = cpu_to_le64(links);
    attributes->birth_time_ns = cpu_to_le64(now);
    attributes->access_time_ns = cpu_to_le64(now);
    attributes->modification_time_ns = cpu_to_le64(now);
    attributes->change_time_ns = cpu_to_le64(now);
    posix->permissions = cpu_to_le32(mode & 07777);
    posix->uid = cpu_to_le32(uid);
    posix->gid = cpu_to_le32(gid);
}

/*
 * Namespace symlink creation is compiled after the legacy layer and can use
 * the credential-aware initializer directly.  Regular-file and directory
 * creation still pass through the qualified legacy transaction and are fixed
 * atomically by the POSIX wrapper below immediately after creation.
 */
#define infilfs_rw_fill_common_attributes infilfs_posix_fill_common_attributes
#include "infiltratorfs_rw_namespace.inc"
#undef infilfs_rw_fill_common_attributes

static int infilfs_posix_object_fields(
    u8 object[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_attributes_disk **attributes_out,
    struct infilfs_posix_compat_disk **posix_out)
{
    struct infilfs_object_header_disk *header =
        (struct infilfs_object_header_disk *)object;

    switch (le16_to_cpu(header->object_type)) {
    case INFILFS_OBJECT_DIRECTORY: {
        struct infilfs_directory_payload_disk *payload =
            (struct infilfs_directory_payload_disk *)(header + 1);
        *attributes_out = &payload->attributes;
        *posix_out = &payload->posix;
        return 0;
    }
    case INFILFS_OBJECT_FILE: {
        struct infilfs_file_payload_disk *payload =
            (struct infilfs_file_payload_disk *)(header + 1);
        *attributes_out = &payload->attributes;
        *posix_out = &payload->posix;
        return 0;
    }
    case INFILFS_OBJECT_SYMLINK: {
        struct infilfs_symlink_payload_disk *payload =
            (struct infilfs_symlink_payload_disk *)(header + 1);
        *attributes_out = &payload->attributes;
        *posix_out = &payload->posix;
        return 0;
    }
    default:
        return -EOPNOTSUPP;
    }
}

static __le64 infilfs_posix_encode_time(struct timespec64 value)
{
    return cpu_to_le64((u64)timespec64_to_ns(&value));
}

/*
 * Copy-on-write one inode's POSIX metadata and repoint its object-index entry
 * in the same transaction.  This is the common persistence primitive for
 * chmod(2), chown(2), utimensat(2)/touch(1), and creator ownership repair.
 */
static int infilfs_posix_rewrite_inode(struct inode *inode,
                                       const struct iattr *attr)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_native_pending *pending = NULL;
    struct infilfs_object_header_disk *header;
    struct infilfs_attributes_disk *attributes;
    struct infilfs_posix_compat_disk *posix;
    struct infilfs_ns_index_change change;
    u8 *object = NULL;
    u64 old_block;
    u64 new_block = 0;
    u32 value;
    bool metadata_changed = false;
    int ret;

    if (!ii)
        return -EIO;
    if (!(attr->ia_valid & (ATTR_MODE | ATTR_UID | ATTR_GID |
                            ATTR_ATIME | ATTR_MTIME | ATTR_CTIME)))
        return 0;

    ret = infilfs_ns_begin(inode->i_sb, &pending);
    if (ret)
        return ret;

    old_block = ii->object_block;
    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object) {
        ret = -ENOMEM;
        goto finish;
    }
    ret = infilfs_read_object(inode->i_sb, old_block, ii->object_type,
                              ii->object_id, object);
    if (ret)
        goto finish;
    header = (struct infilfs_object_header_disk *)object;
    ret = infilfs_posix_object_fields(object, &attributes, &posix);
    if (ret)
        goto finish;

    if (attr->ia_valid & ATTR_MODE) {
        posix->permissions = cpu_to_le32(attr->ia_mode & 07777);
        metadata_changed = true;
    }
    if (attr->ia_valid & ATTR_UID) {
        ret = infilfs_posix_uid_to_disk(attr->ia_uid, &value);
        if (ret)
            goto finish;
        posix->uid = cpu_to_le32(value);
        metadata_changed = true;
    }
    if (attr->ia_valid & ATTR_GID) {
        ret = infilfs_posix_gid_to_disk(attr->ia_gid, &value);
        if (ret)
            goto finish;
        posix->gid = cpu_to_le32(value);
        metadata_changed = true;
    }
    if (attr->ia_valid & ATTR_ATIME) {
        attributes->access_time_ns = infilfs_posix_encode_time(attr->ia_atime);
        metadata_changed = true;
    }
    if (attr->ia_valid & ATTR_MTIME) {
        attributes->modification_time_ns =
            infilfs_posix_encode_time(attr->ia_mtime);
        metadata_changed = true;
    }
    if (attr->ia_valid & ATTR_CTIME) {
        attributes->change_time_ns = infilfs_posix_encode_time(attr->ia_ctime);
    } else if (metadata_changed) {
        attributes->change_time_ns = cpu_to_le64(ktime_get_real_ns());
    }

    header->generation = cpu_to_le64(pending->tx.generation);
    ret = infilfs_rw_finalize_object(object);
    if (ret)
        goto finish;
    ret = infilfs_rw_tx_alloc(&pending->tx, 1, &new_block);
    if (ret)
        goto finish;
    ret = infilfs_native_stage_block(inode->i_sb, new_block, object);
    if (ret)
        goto finish;
    ret = infilfs_rw_tx_defer_free(&pending->tx, old_block, 1);
    if (ret)
        goto finish;

    memset(&change, 0, sizeof(change));
    memcpy(change.object_id, ii->object_id, sizeof(change.object_id));
    change.object_block = new_block;
    change.object_type = ii->object_type;
    change.action = INFILFS_NS_REPOINT;

    if (memcmp(ii->object_id, pending->tx.sbi->disk.root_object_id, 16) == 0)
        pending->tx.next_sb.root_object_block = cpu_to_le64(new_block);

    ret = infilfs_ns_rebuild_index(pending, &change, 1);

finish:
    kfree(object);
    ret = infilfs_ns_finish(pending, ret);
    if (!ret)
        ii->object_block = new_block;
    return ret;
}

/*
 * Finish the ownership semantics that inode_init_owner() would normally apply
 * during VFS creation.  The existing qualified create transaction has already
 * preserved the requested umask-filtered mode; we add the creator uid and the
 * correct fsgid/setgid-directory inheritance and persist them before returning
 * success to the VFS.
 */
static int infilfs_posix_fix_created(struct inode *parent,
                                     struct dentry *dentry,
                                     bool is_directory)
{
    struct inode *inode = d_inode(dentry);
    struct iattr attr;
    int ret;
    kuid_t creator_uid;
    kgid_t creator_gid;

    if (!inode)
        return -EIO;

    memset(&attr, 0, sizeof(attr));
    infilfs_posix_current_ids(&creator_uid, &creator_gid);
    attr.ia_valid = ATTR_UID | ATTR_GID | ATTR_CTIME;
    attr.ia_uid = creator_uid;
    attr.ia_gid = (parent->i_mode & S_ISGID) ? parent->i_gid : creator_gid;
    attr.ia_ctime = current_time(inode);

    if (is_directory && (parent->i_mode & S_ISGID)) {
        attr.ia_valid |= ATTR_MODE;
        attr.ia_mode = (inode->i_mode & 07777) | S_ISGID;
    }

    ret = infilfs_posix_rewrite_inode(inode, &attr);
    if (ret)
        return ret;

    inode->i_uid = attr.ia_uid;
    inode->i_gid = attr.ia_gid;
    if (attr.ia_valid & ATTR_MODE)
        inode->i_mode = (inode->i_mode & S_IFMT) | (attr.ia_mode & 07777);
    return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_create(struct mnt_idmap *idmap, struct inode *dir,
                                struct dentry *dentry, umode_t mode, bool excl)
#else
static int infilfs_posix_create(struct user_namespace *idmap,
                                struct inode *dir, struct dentry *dentry,
                                umode_t mode, bool excl)
#endif
{
    int ret = infilfs_rw_create_data(idmap, dir, dentry, mode, excl);

    if (ret)
        return ret;
    return infilfs_posix_fix_created(dir, dentry, false);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
static struct dentry *infilfs_posix_mkdir(struct mnt_idmap *idmap,
                                          struct inode *dir,
                                          struct dentry *dentry,
                                          umode_t mode)
{
    struct dentry *result = infilfs_rw_mkdir_data(idmap, dir, dentry, mode);
    struct dentry *created;
    int ret;

    if (IS_ERR(result))
        return result;
    created = result ? result : dentry;
    ret = infilfs_posix_fix_created(dir, created, true);
    if (ret)
        return ERR_PTR(ret);
    return result;
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_mkdir(struct mnt_idmap *idmap, struct inode *dir,
                               struct dentry *dentry, umode_t mode)
{
    int ret = infilfs_rw_mkdir_data(idmap, dir, dentry, mode);

    if (ret)
        return ret;
    return infilfs_posix_fix_created(dir, dentry, true);
}
#else
static int infilfs_posix_mkdir(struct user_namespace *idmap,
                               struct inode *dir, struct dentry *dentry,
                               umode_t mode)
{
    int ret = infilfs_rw_mkdir_data(idmap, dir, dentry, mode);

    if (ret)
        return ret;
    return infilfs_posix_fix_created(dir, dentry, true);
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_symlink(struct mnt_idmap *idmap, struct inode *dir,
                                 struct dentry *dentry, const char *target)
#else
static int infilfs_posix_symlink(struct user_namespace *idmap,
                                 struct inode *dir, struct dentry *dentry,
                                 const char *target)
#endif
{
    int ret = infilfs_ns_symlink(idmap, dir, dentry, target);

    if (ret)
        return ret;
    return infilfs_posix_fix_created(dir, dentry, false);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_setattr(struct mnt_idmap *idmap,
                                 struct dentry *dentry,
                                 struct iattr *attr)
#else
static int infilfs_posix_setattr(struct user_namespace *idmap,
                                 struct dentry *dentry,
                                 struct iattr *attr)
#endif
{
    struct inode *inode = d_inode(dentry);
    struct iattr metadata;
    unsigned int disk_fields;
    int ret;

    ret = setattr_prepare(idmap, dentry, attr);
    if (ret)
        return ret;
    metadata = *attr;

    /*
     * Keep the already-qualified size path intact.  It currently supports the
     * inline truncate range; metadata fields that accompany ATTR_SIZE are then
     * persisted in the POSIX transaction below.
     */
    if (attr->ia_valid & ATTR_SIZE) {
        if (!S_ISREG(inode->i_mode))
            return -EISDIR;
        ret = infilfs_rw_setattr_data(idmap, dentry, attr);
        if (ret)
            return ret;
        metadata.ia_valid &= ~ATTR_SIZE;
    }

    disk_fields = metadata.ia_valid &
        (ATTR_MODE | ATTR_UID | ATTR_GID | ATTR_ATIME | ATTR_MTIME | ATTR_CTIME);
    if (!disk_fields)
        return 0;

    ret = infilfs_posix_rewrite_inode(inode, &metadata);
    if (ret)
        return ret;

    /*
     * Keep Linux's in-memory inode authoritative for permission checks between
     * metadata transactions.  getattr still reports the persisted timestamps
     * from disk, so both views agree after this copy.
     */
    setattr_copy(idmap, inode, &metadata);
    return 0;
}

/*
 * Native VFS metadata bridge.
 *
 * InfiltratorFS stores all four timestamps, including birth time, in its
 * portable on-disk attributes.  Report those authoritative values through
 * ->getattr so desktop file managers see creation time as well as POSIX times.
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
    /*
     * Object count has no fixed inode table; these are conservative upper
     * bounds used only for generic VFS reporting.
     */
    buf->f_files = total;
    buf->f_ffree = free_blocks;
    buf->f_namelen = INFILFS_NAME_MAX;
    return 0;
}

/*
 * infiltratorfs.c declares the operation tables immediately after including
 * this composition file.  Supply the final native callbacks here while keeping
 * that table compact.
 */
#define simple_statfs infilfs_statfs
#define infilfs_rw_create infilfs_posix_create
#define infilfs_rw_mkdir infilfs_posix_mkdir, \
    .unlink = infilfs_ns_unlink, \
    .rmdir = infilfs_ns_rmdir, \
    .link = infilfs_ns_link, \
    .symlink = infilfs_posix_symlink, \
    .rename = infilfs_ns_rename, \
    .setattr = infilfs_posix_setattr, \
    .getattr = infilfs_getattr
#define infilfs_rw_setattr infilfs_posix_setattr, .getattr = infilfs_getattr
#define infilfs_get_link infilfs_get_link, \
    .setattr = infilfs_posix_setattr, \
    .getattr = infilfs_getattr
