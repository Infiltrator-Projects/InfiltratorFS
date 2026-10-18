// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"

/* Current-format checkpoint validation and bounded locator discovery. */
static const u8 infilfs_disk_magic[8] = {
    'I', 'N', 'F', 'S', '2', '0', '2', '6'
};

static bool infilfs_checkpoint_basic_valid(
    const struct infilfs_superblock_disk *disk, u64 device_blocks)
{
    const u8 *label_end;
    u64 total;
    u64 incompat;
    u64 checkpoint[INFILFS_CHECKPOINT_COUNT];
    unsigned int i;
    unsigned int j;

    if (memcmp(disk->magic, infilfs_disk_magic,
               sizeof(infilfs_disk_magic)) != 0)
        return false;
    if (le16_to_cpu(disk->format_major) != INFILFS_FORMAT_MAJOR ||
        le16_to_cpu(disk->format_minor) != INFILFS_FORMAT_MINOR ||
        le16_to_cpu(disk->header_size) != sizeof(*disk) ||
        le16_to_cpu(disk->block_shift) != INFILFS_DISK_BLOCK_SHIFT ||
        le32_to_cpu(disk->checksum_type) != INFILFS_CHECKSUM_CRC64_ECMA ||
        le64_to_cpu(disk->generation) == 0)
        return false;

    total = le64_to_cpu(disk->total_blocks);
    if (total < 3 || total > device_blocks ||
        le64_to_cpu(disk->free_blocks) > total)
        return false;

    for (i = 0; i < INFILFS_CHECKPOINT_COUNT; ++i) {
        checkpoint[i] = le64_to_cpu(disk->checkpoint_block[i]);
        if (checkpoint[i] >= total)
            return false;
    }
    if (checkpoint[0] != 0)
        return false;
    for (i = 0; i < INFILFS_CHECKPOINT_COUNT; ++i)
        for (j = i + 1; j < INFILFS_CHECKPOINT_COUNT; ++j)
            if (checkpoint[i] == checkpoint[j])
                return false;

    {
        size_t leaves = 0;
        if (infilfs_allocation_counts(total, &leaves, NULL, NULL, NULL) ||
            le64_to_cpu(disk->allocation_leaf_count) != leaves ||
            !le64_to_cpu(disk->allocation_root_block) ||
            le64_to_cpu(disk->allocation_root_block) >= total ||
            le64_to_cpu(disk->object_index_block) >= total ||
            le64_to_cpu(disk->root_object_block) >= total)
            return false;
    }

    incompat = le64_to_cpu(disk->incompat_flags);
    if (incompat & ~INFILFS_KNOWN_INCOMPAT_FLAGS)
        return false;
    if ((incompat & (INFILFS_INCOMPAT_UTF8_NAMES |
                     INFILFS_INCOMPAT_SPARSE_EXTENTS |
                     INFILFS_INCOMPAT_ALLOCATION_TREE |
                     INFILFS_INCOMPAT_UNICODE_NORM_V1)) !=
        (INFILFS_INCOMPAT_UTF8_NAMES | INFILFS_INCOMPAT_SPARSE_EXTENTS |
         INFILFS_INCOMPAT_ALLOCATION_TREE |
         INFILFS_INCOMPAT_UNICODE_NORM_V1))
        return false;
    if (!memchr_inv(disk->filesystem_uuid, 0,
                    sizeof(disk->filesystem_uuid)) ||
        !memchr_inv(disk->root_object_id, 0,
                    sizeof(disk->root_object_id)))
        return false;
    label_end = memchr(disk->label, 0, sizeof(disk->label));
    if (!label_end ||
        memchr_inv(label_end, 0,
                   sizeof(disk->label) -
                       (size_t)(label_end - disk->label)) ||
        !infilfs_rw_utf8_valid(
            disk->label, (size_t)(label_end - disk->label)))
        return false;

    return true;
}

/* Two fixed maps plus an independently validated block-zero map. Never
 * guess midpoint/end positions when no trustworthy map survives. */
int infilfs_read_checkpoint_candidates(
    struct super_block *sb, struct infilfs_checkpoint_candidates *set)
{
    u64 device_blocks = INFILFS_SB(sb)->device_blocks;
    struct infilfs_recovery_locator maps[3] = {0};
    const u64 fixed[2] = {INFILFS_LOCATOR_BLOCK_A, INFILFS_LOCATOR_BLOCK_B};
    struct infilfs_superblock_disk disk;
    unsigned mask = 0, found = 0, i, j;
    u8 *raw;
    int ret = 0;
    memset(set, 0, sizeof(*set));
    set->format019 = true;
    if (device_blocks < INFILFS_LOCATOR_MIN_VOLUME_BLOCKS)
        return -EFSCORRUPTED;
    raw = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!raw)
        return -ENOMEM;
    for (i = 0; i < 3u; ++i) {
        ret = infilfs_read_block(sb, i < 2u ? fixed[i] : 0, raw);
        if (ret) {
            if (!set->first_read_error)
                set->first_read_error = ret;
            continue;
        }
        if (i < 2u) {
            if (!infilfs_locator_decode_019(raw, device_blocks, &maps[i]))
                mask |= 1u << i;
        } else {
            bool valid = true;
            if (!infilfs_crc64_block_valid(raw,
                    offsetof(struct infilfs_superblock_disk, checksum),
                    sizeof(disk.checksum)) ||
                memchr_inv(raw + sizeof(disk), 0,
                           INFILFS_DISK_BLOCK_SIZE - sizeof(disk)))
                continue;
            memcpy(&disk, raw, sizeof(disk));
            if (!infilfs_checkpoint_basic_valid(&disk, device_blocks) ||
                le16_to_cpu(disk.format_minor) != INFILFS_LOCATOR_FORMAT_MINOR ||
                le64_to_cpu(disk.total_blocks) < INFILFS_LOCATOR_MIN_VOLUME_BLOCKS)
                continue;
            maps[i].generation = le64_to_cpu(disk.generation);
            maps[i].total_blocks = le64_to_cpu(disk.total_blocks);
            memcpy(maps[i].filesystem_uuid, disk.filesystem_uuid, 16u);
            for (j = 0; j < INFILFS_CHECKPOINT_COUNT; ++j) {
                u64 block = le64_to_cpu(disk.checkpoint_block[j]);
                maps[i].checkpoint[j] = block;
                if (block == fixed[0] || block == fixed[1])
                    valid = false;
            }
            if (valid)
                mask |= 1u << i;
        }
    }
    for (i = 0; i < 3u; ++i) {
        if (!(mask & (1u << i)))
            continue;
        for (j = i + 1u; j < 3u; ++j) {
            if (!(mask & (1u << j)))
                continue;
            if (memcmp(maps[i].filesystem_uuid, maps[j].filesystem_uuid, 16u)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            if (j == 2u && maps[i].total_blocks == maps[j].total_blocks &&
                !memcmp(maps[i].checkpoint, maps[j].checkpoint,
                        sizeof(maps[i].checkpoint)))
                mask &= ~(1u << j);
        }
    }
    for (i = 0; i < 3u; ++i) {
        if (!(mask & (1u << i)))
            continue;
        for (j = 0; j < INFILFS_CHECKPOINT_COUNT; ++j) {
            unsigned idx = i * INFILFS_CHECKPOINT_COUNT + j, c;
            bool consistent = true;
            if (set->count <= idx)
                set->count = idx + 1u;
            ret = infilfs_read_block(sb, maps[i].checkpoint[j], raw);
            if (ret) {
                if (!set->first_read_error)
                    set->first_read_error = ret;
                continue;
            }
            if (!infilfs_crc64_block_valid(raw,
                    offsetof(struct infilfs_superblock_disk, checksum),
                    sizeof(disk.checksum)) ||
                memchr_inv(raw + sizeof(disk), 0,
                           INFILFS_DISK_BLOCK_SIZE - sizeof(disk)))
                continue;
            memcpy(&disk, raw, sizeof(disk));
            if (!infilfs_checkpoint_basic_valid(&disk, device_blocks) ||
                le16_to_cpu(disk.format_minor) != INFILFS_LOCATOR_FORMAT_MINOR ||
                le64_to_cpu(disk.total_blocks) != maps[i].total_blocks ||
                memcmp(disk.filesystem_uuid, maps[i].filesystem_uuid, 16u))
                continue;
            for (c = 0; c < INFILFS_CHECKPOINT_COUNT; ++c)
                if (le64_to_cpu(disk.checkpoint_block[c]) != maps[i].checkpoint[c])
                    consistent = false;
            if (!consistent)
                continue;
            set->disks[idx] = disk;
            set->valid[idx] = true;
            ++found;
        }
    }
    ret = found ? 0 : -EFSCORRUPTED;
    if (set->first_read_error && (!sb_rdonly(sb) || !found))
        ret = set->first_read_error;
out:
    kfree(raw);
    return ret;
}
