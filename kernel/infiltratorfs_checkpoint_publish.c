// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"
#include "checkpoint_protocol.h"

struct infilfs_checkpoint_publication {
    struct super_block *sb;
    const struct infilfs_superblock_disk *disk;
    const u8 *encoded;
};

/* Complete one buffer write. The shared protocol then issues the device
 * cache barrier; I/O completion alone does not establish durability. */
int infilfs_checkpoint_write_sync(
    struct super_block *sb, u64 block, const void *data)
{
    struct buffer_head *bh;
    int ret = 0;
    if (block >= INFILFS_SB(sb)->device_blocks)
        return -EIO;
    bh = sb_getblk(sb, (sector_t)block);
    if (!bh)
        return -ENOMEM;
    lock_buffer(bh);
    memcpy(bh->b_data, data, INFILFS_DISK_BLOCK_SIZE);
    clear_buffer_write_io_error(bh);
    set_buffer_uptodate(bh);
    mark_buffer_dirty(bh);
    unlock_buffer(bh);
    write_dirty_buffer(bh, REQ_SYNC);
    wait_on_buffer(bh);
    if (buffer_write_io_error(bh) || !buffer_uptodate(bh))
        ret = -EIO;
    brelse(bh);
    return ret;
}

static int infilfs_publish_flush(void *context)
{
    struct infilfs_checkpoint_publication *p = context;
    return blkdev_issue_flush(p->sb->s_bdev);
}

static int infilfs_publish_checkpoint(void *context, unsigned copy)
{
    struct infilfs_checkpoint_publication *p = context;
    return infilfs_checkpoint_write_sync(p->sb,
        le64_to_cpu(p->disk->checkpoint_block[copy]), p->encoded);
}

static int infilfs_publish_locator(void *context, unsigned copy)
{
    struct infilfs_checkpoint_publication *p = context;
    struct infilfs_recovery_locator locator = {0};
    u8 *raw;
    unsigned i;
    int ret;
    raw = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!raw)
        return -ENOMEM;
    locator.generation = le64_to_cpu(p->disk->generation);
    locator.total_blocks = le64_to_cpu(p->disk->total_blocks);
    memcpy(locator.filesystem_uuid, p->disk->filesystem_uuid, 16u);
    for (i = 0; i < INFILFS_CHECKPOINT_COUNT; ++i)
        locator.checkpoint[i] = le64_to_cpu(p->disk->checkpoint_block[i]);
    ret = infilfs_locator_encode_019(&locator, raw);
    if (!ret)
        ret = infilfs_checkpoint_write_sync(p->sb,
            copy ? INFILFS_LOCATOR_BLOCK_B : INFILFS_LOCATOR_BLOCK_A, raw);
    kfree(raw);
    return ret;
}

static const struct infs_checkpoint_io infilfs_publication_io = {
    .locator = infilfs_publish_locator,
    .checkpoint = infilfs_publish_checkpoint,
    .flush = infilfs_publish_flush,
};

int infilfs_checkpoint_publish(struct super_block *sb,
    const struct infilfs_superblock_disk *disk, const u8 *encoded,
    unsigned first, bool relocate, int *committed)
{
    struct infilfs_checkpoint_publication p = {sb, disk, encoded};
    return infs_checkpoint_publish(&infilfs_publication_io,
        &p, first, relocate, committed);
}

/* Keep every matching, fully validated survivor intact while healing others.
 * Only after all checkpoint copies are durable may obsolete locator hints
 * be replaced; a crash during repair always retains a discoverable graph. */
int infilfs_checkpoint_heal(struct super_block *sb, bool promoting)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct infilfs_checkpoint_publication p;
    u8 *raw, *encoded;
    unsigned i;
    int ret = 0;
    if (sb_rdonly(sb) && !promoting)
        return 0;
    raw = kmalloc(INFILFS_DISK_BLOCK_SIZE * 2u, GFP_NOFS);
    if (!raw)
        return -ENOMEM;
    encoded = raw + INFILFS_DISK_BLOCK_SIZE;
    infilfs_rw_encode_superblock(&sbi->disk, encoded);
    p.sb = sb; p.disk = &sbi->disk; p.encoded = encoded;
    for (i = 0; i < INFILFS_CHECKPOINT_COUNT; ++i) {
        ret = infilfs_read_block(sb,
            le64_to_cpu(sbi->disk.checkpoint_block[i]), raw);
        if (ret)
            goto out;
        if (!memcmp(raw, encoded, INFILFS_DISK_BLOCK_SIZE))
            continue;
        ret = infilfs_publish_checkpoint(&p, i);
        if (!ret)
            ret = infilfs_publish_flush(&p);
        if (ret)
            goto out;
    }
    if (le16_to_cpu(sbi->disk.format_minor) == INFILFS_LOCATOR_FORMAT_MINOR) {
        for (i = 0; i < 2u; ++i) {
            struct infilfs_recovery_locator loc;
            bool matches;
            unsigned c;
            ret = infilfs_read_block(sb,
                i ? INFILFS_LOCATOR_BLOCK_B : INFILFS_LOCATOR_BLOCK_A, raw);
            if (ret)
                goto out;
            matches = !infilfs_locator_decode_019(raw, sbi->device_blocks, &loc) &&
                loc.total_blocks == le64_to_cpu(sbi->disk.total_blocks) &&
                loc.generation <= le64_to_cpu(sbi->disk.generation) &&
                !memcmp(loc.filesystem_uuid, sbi->disk.filesystem_uuid, 16u);
            for (c = 0; matches && c < INFILFS_CHECKPOINT_COUNT; ++c)
                matches = loc.checkpoint[c] ==
                    le64_to_cpu(sbi->disk.checkpoint_block[c]);
            if (matches)
                continue;
            ret = infilfs_publish_locator(&p, i);
            if (!ret)
                ret = infilfs_publish_flush(&p);
            if (ret)
                goto out;
        }
    }
out:
    if (ret) {
        sbi->write_poisoned = true;
        sbi->checkpoint_repair_needed = true;
    } else {
        sbi->checkpoint_repair_needed = false;
    }
    kfree(raw);
    return ret;
}
