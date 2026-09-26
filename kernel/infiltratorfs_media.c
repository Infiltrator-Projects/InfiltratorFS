// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"

/*
 * Media-profile admission and naming are isolated from mount orchestration so
 * the core translation unit does not grow every time media policy evolves.
 * Format 0.18 allocation remains conventional-block-addressed; zoned devices
 * therefore fail closed until a zone-aware allocator exists.
 */

const char *infilfs_media_profile_name(
    enum infilfs_media_profile profile)
{
    switch (profile) {
    case INFILFS_MEDIA_ROTATIONAL:
        return "rotational";
    case INFILFS_MEDIA_NONROTATIONAL:
        return "nonrotational";
    case INFILFS_MEDIA_BALANCED:
    default:
        return "balanced";
    }
}

int infilfs_resolve_media_profile(
    struct super_block *sb, const struct infilfs_fs_context *ctx,
    struct infilfs_sb_info *sbi)
{
    struct request_queue *queue;

    if (!sb || !sb->s_bdev || !sbi)
        return -EINVAL;
    queue = bdev_get_queue(sb->s_bdev);
    if (queue && blk_queue_is_zoned(queue)) {
        pr_err("InfiltratorFS: zoned block devices require zone-aware allocation and are not yet supported\n");
        return -EOPNOTSUPP;
    }

    sbi->media_profile_overridden =
        ctx && ctx->media_override != INFILFS_MEDIA_OVERRIDE_AUTO;
    if (ctx) {
        switch (ctx->media_override) {
        case INFILFS_MEDIA_OVERRIDE_BALANCED:
            sbi->media_profile = INFILFS_MEDIA_BALANCED;
            return 0;
        case INFILFS_MEDIA_OVERRIDE_ROTATIONAL:
            sbi->media_profile = INFILFS_MEDIA_ROTATIONAL;
            return 0;
        case INFILFS_MEDIA_OVERRIDE_NONROTATIONAL:
            sbi->media_profile = INFILFS_MEDIA_NONROTATIONAL;
            return 0;
        case INFILFS_MEDIA_OVERRIDE_AUTO:
        default:
            break;
        }
    }

    if (!queue) {
        sbi->media_profile = INFILFS_MEDIA_BALANCED;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
    } else if (bdev_rot(sb->s_bdev)) {
        sbi->media_profile = INFILFS_MEDIA_ROTATIONAL;
#else
    } else if (!blk_queue_nonrot(queue)) {
        sbi->media_profile = INFILFS_MEDIA_ROTATIONAL;
#endif
    } else {
        sbi->media_profile = INFILFS_MEDIA_NONROTATIONAL;
    }
    return 0;
}
