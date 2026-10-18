// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"

/* Format-independent checkpoint validation and separate 0.18/0.19 discovery. */
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
        (le16_to_cpu(disk->format_minor) != INFILFS_FORMAT_MINOR &&
         le16_to_cpu(disk->format_minor) != INFILFS_LOCATOR_FORMAT_MINOR) ||
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
    if (le64_to_cpu(disk->compat_flags) != 0 ||
        le64_to_cpu(disk->ro_compat_flags) != 0)
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

/* Keep both independent Format 0.19 locator maps across interrupted resize. */
int infilfs_read_checkpoint_candidates(
    struct super_block *sb, struct infilfs_checkpoint_candidates *set)
{
    u64 device_blocks = INFILFS_SB(sb)->device_blocks;
    u64 old_candidates[INFILFS_CHECKPOINT_COUNT] = {
        0, device_blocks / 2, device_blocks - 1
    };
    struct infilfs_superblock_disk anchor;
    struct infilfs_superblock_disk current;
    struct infilfs_recovery_locator locators[2] = {0};
    bool anchored = false;
    unsigned int found = 0;
    unsigned int mask = 0;
    int first_locator_error = 0;
    unsigned int i, j;
    u8 *raw;

    memset(set, 0, sizeof(*set));
    raw = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!raw)
        return -ENOMEM;

    /* Format 0.19 never trusts physical midpoint/end fallback. */
    if (device_blocks >= INFILFS_LOCATOR_MIN_VOLUME_BLOCKS) {
        static const u64 addresses[2] = {
            INFILFS_LOCATOR_BLOCK_A, INFILFS_LOCATOR_BLOCK_B
        };
        for (i = 0; i < 2; ++i) {
            int ret = infilfs_read_block(sb, addresses[i], raw);
            if (ret) {
                if (!first_locator_error)
                    first_locator_error = ret;
                continue;
            }
            if (!infilfs_locator_decode_019(
                    raw, device_blocks, &locators[i]))
                mask |= 1u << i;
        }
    }

    if (!mask && device_blocks >= INFILFS_LOCATOR_MIN_VOLUME_BLOCKS) {
        /* A valid primary 0.19 checkpoint is an independent source of
         * bounded backup locations when BOTH fixed locators are lost.
         * Never guess the current midpoint/end and never trust this header
         * without the normal full committed-graph verification. */
        int ret = infilfs_read_block(sb, 0, raw);
        if (ret) {
            if (!first_locator_error)
                first_locator_error = ret;
        } else if (infilfs_crc64_block_valid(
                       raw, offsetof(struct infilfs_superblock_disk,
                                     checksum),
                       sizeof(((struct infilfs_superblock_disk *)raw)->
                              checksum)) &&
                   !memchr_inv(raw + sizeof(anchor), 0,
                               INFILFS_DISK_BLOCK_SIZE - sizeof(anchor))) {
            bool admissible = true;
            memcpy(&anchor, raw, sizeof(anchor));
            if (infilfs_checkpoint_basic_valid(&anchor, device_blocks) &&
                le16_to_cpu(anchor.format_minor) ==
                    INFILFS_LOCATOR_FORMAT_MINOR &&
                le64_to_cpu(anchor.total_blocks) >=
                    INFILFS_LOCATOR_MIN_VOLUME_BLOCKS) {
                locators[0].generation = le64_to_cpu(anchor.generation);
                locators[0].total_blocks =
                    le64_to_cpu(anchor.total_blocks);
                memcpy(locators[0].filesystem_uuid,
                       anchor.filesystem_uuid, 16u);
                for (i = 0; i < INFILFS_CHECKPOINT_COUNT; ++i) {
                    u64 pos = le64_to_cpu(anchor.checkpoint_block[i]);
                    locators[0].checkpoint[i] = pos;
                    if (pos == INFILFS_LOCATOR_BLOCK_A ||
                        pos == INFILFS_LOCATOR_BLOCK_B)
                        admissible = false;
                }
                if (admissible)
                    mask = 1u;
            }
        }
    }

    if (mask) {
        set->format019 = true;
        set->first_read_error = first_locator_error;
        if (mask == 3u &&
            memcmp(locators[0].filesystem_uuid,
                   locators[1].filesystem_uuid, 16u)) {
            kfree(raw);
            return -EFSCORRUPTED;
        }
        for (i = 0; i < 2; ++i) {
            if (!(mask & (1u << i)))
                continue;
            for (j = 0; j < INFILFS_CHECKPOINT_COUNT; ++j) {
                unsigned int idx = i * INFILFS_CHECKPOINT_COUNT + j;
                const struct infilfs_recovery_locator *hint = &locators[i];
                u64 pos = hint->checkpoint[j];
                bool consistent = true;
                unsigned int k;
                int ret = infilfs_read_block(sb, pos, raw);

                if (ret) {
                    if (!set->first_read_error)
                        set->first_read_error = ret;
                    continue;
                }
                if (!infilfs_crc64_block_valid(
                        raw, offsetof(struct infilfs_superblock_disk,
                                      checksum),
                        sizeof(((struct infilfs_superblock_disk *)raw)->
                               checksum)) ||
                    memchr_inv(raw + sizeof(current), 0,
                               INFILFS_DISK_BLOCK_SIZE -
                               sizeof(current)))
                    continue;
                memcpy(&current, raw, sizeof(current));
                if (!infilfs_checkpoint_basic_valid(
                        &current, device_blocks) ||
                    le16_to_cpu(current.format_minor) !=
                        INFILFS_LOCATOR_FORMAT_MINOR ||
                    le64_to_cpu(current.total_blocks) != hint->total_blocks ||
                    memcmp(current.filesystem_uuid,
                           hint->filesystem_uuid, 16u))
                    continue;
                for (k = 0; k < INFILFS_CHECKPOINT_COUNT; ++k)
                    if (le64_to_cpu(current.checkpoint_block[k]) !=
                        hint->checkpoint[k])
                        consistent = false;
                if (!consistent)
                    continue;
                set->disks[idx] = current;
                set->valid[idx] = true;
                found++;
            }
        }
        goto done;
    }

    /* Legacy geometry discovery accepts only Format 0.18. */
    {
        int ret = infilfs_read_block(sb, 0, raw);
        if (!ret &&
            infilfs_crc64_block_valid(
                raw, offsetof(struct infilfs_superblock_disk, checksum),
                sizeof(((struct infilfs_superblock_disk *)raw)->checksum)) &&
            !memchr_inv(raw + sizeof(anchor), 0,
                        INFILFS_DISK_BLOCK_SIZE - sizeof(anchor))) {
            memcpy(&anchor, raw, sizeof(anchor));
            if (infilfs_checkpoint_basic_valid(&anchor, device_blocks) &&
                le16_to_cpu(anchor.format_minor) == INFILFS_FORMAT_MINOR) {
                for (i = 0; i < INFILFS_CHECKPOINT_COUNT; ++i)
                    old_candidates[i] =
                        le64_to_cpu(anchor.checkpoint_block[i]);
                anchored = true;
            }
        } else if (ret && !set->first_read_error) {
            set->first_read_error = ret;
        }
    }

    for (i = 0; i < INFILFS_CHECKPOINT_COUNT; ++i) {
        int ret = infilfs_read_block(sb, old_candidates[i], raw);

        if (ret) {
            if (!set->first_read_error)
                set->first_read_error = ret;
            continue;
        }
        if (!infilfs_crc64_block_valid(
                raw, offsetof(struct infilfs_superblock_disk, checksum),
                sizeof(((struct infilfs_superblock_disk *)raw)->checksum)) ||
            memchr_inv(raw + sizeof(current), 0,
                       INFILFS_DISK_BLOCK_SIZE - sizeof(current)))
            continue;
        memcpy(&current, raw, sizeof(current));
        if (!infilfs_checkpoint_basic_valid(&current, device_blocks) ||
            le16_to_cpu(current.format_minor) != INFILFS_FORMAT_MINOR)
            continue;
        for (j = 0; j < INFILFS_CHECKPOINT_COUNT; ++j)
            if (le64_to_cpu(current.checkpoint_block[j]) !=
                old_candidates[j])
                break;
        if (j != INFILFS_CHECKPOINT_COUNT)
            continue;
        if (anchored &&
            (le64_to_cpu(current.total_blocks) !=
                 le64_to_cpu(anchor.total_blocks) ||
             memcmp(current.filesystem_uuid, anchor.filesystem_uuid,
                    sizeof(current.filesystem_uuid))))
            continue;
        set->disks[i] = current;
        set->valid[i] = true;
        found++;
    }
done:
    kfree(raw);
    if (set->first_read_error && !sb_rdonly(sb))
        return set->first_read_error;
    if (found)
        return 0;
    if (first_locator_error && !set->first_read_error)
        set->first_read_error = first_locator_error;
    return set->first_read_error ?
        set->first_read_error : -EFSCORRUPTED;
}

