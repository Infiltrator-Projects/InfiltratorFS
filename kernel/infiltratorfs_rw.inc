// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Native Linux RW composition.
 *
 * Keep the already-qualified inline transaction engine intact as the legacy
 * implementation, then layer the extent-backed/deferred native data path over
 * the VFS entry points that need it.  This keeps the bring-up code reviewable
 * while native parity is expanded operation by operation.
 */

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
