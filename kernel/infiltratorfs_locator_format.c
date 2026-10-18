// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"

/* Parallel to portable src/locator.c; the kernel does not link userspace
 * Common or its C17 storage implementation. These are locator *hints*, never
 * an authority for committing or selecting a generation without full graph
 * validation. */
static const u8 infilfs_locator_magic[8] = {
    'I', 'N', 'F', 'S', 'L', '0', '1', '9'
};

static bool infilfs_locator_shape_valid(
    const struct infilfs_recovery_locator *loc)
{
    unsigned int i, j;
    bool nonzero_uuid = false;

    if (!loc || !loc->generation ||
        loc->total_blocks < INFILFS_LOCATOR_MIN_VOLUME_BLOCKS ||
        loc->checkpoint[0] != 0)
        return false;

    for (i = 0; i < 16u; ++i)
        nonzero_uuid |= loc->filesystem_uuid[i] != 0;
    if (!nonzero_uuid)
        return false;

    for (i = 0; i < INFILFS_CHECKPOINT_COUNT; ++i) {
        if (loc->checkpoint[i] >= loc->total_blocks ||
            loc->checkpoint[i] == INFILFS_LOCATOR_BLOCK_A ||
            loc->checkpoint[i] == INFILFS_LOCATOR_BLOCK_B)
            return false;
        for (j = i + 1u; j < INFILFS_CHECKPOINT_COUNT; ++j)
            if (loc->checkpoint[i] == loc->checkpoint[j])
                return false;
    }
    return true;
}

int infilfs_locator_decode_019(const u8 block[4096], u64 device_blocks,
                                struct infilfs_recovery_locator *out)
{
    const struct infilfs_locator_disk *disk;
    struct infilfs_recovery_locator candidate = {0};
    unsigned int i;

    BUILD_BUG_ON(sizeof(struct infilfs_locator_disk) !=
                 INFILFS_LOCATOR_HEADER_BYTES);
    BUILD_BUG_ON(offsetof(struct infilfs_locator_disk, checksum) != 72u);
    if (!block || !out)
        return -EINVAL;
    if (device_blocks < INFILFS_LOCATOR_MIN_VOLUME_BLOCKS)
        return -EFSCORRUPTED;
    disk = (const struct infilfs_locator_disk *)block;
    if (memcmp(disk->magic, infilfs_locator_magic, 8) != 0 ||
        le16_to_cpu(disk->schema) != INFILFS_LOCATOR_SCHEMA_VERSION ||
        le16_to_cpu(disk->header_size) != INFILFS_LOCATOR_HEADER_BYTES ||
        le16_to_cpu(disk->major) != INFILFS_FORMAT_MAJOR ||
        le16_to_cpu(disk->minor) != INFILFS_LOCATOR_FORMAT_MINOR ||
        memchr_inv(block + sizeof(*disk), 0,
                   INFILFS_DISK_BLOCK_SIZE - sizeof(*disk)) ||
        !infilfs_crc64_block_valid(
            block, offsetof(struct infilfs_locator_disk, checksum),
            sizeof(disk->checksum)))
        return -EFSCORRUPTED;

    candidate.generation = le64_to_cpu(disk->generation);
    candidate.total_blocks = le64_to_cpu(disk->total_blocks);
    for (i = 0; i < INFILFS_CHECKPOINT_COUNT; ++i)
        candidate.checkpoint[i] = le64_to_cpu(disk->checkpoints[i]);
    memcpy(candidate.filesystem_uuid, disk->filesystem_uuid, 16u);
    if (!infilfs_locator_shape_valid(&candidate) ||
        candidate.total_blocks > device_blocks)
        return -EFSCORRUPTED;
    *out = candidate;
    return 0;
}

int infilfs_locator_encode_019(
    const struct infilfs_recovery_locator *loc, u8 block[4096])
{
    struct infilfs_locator_disk *disk;
    __le64 checksum;
    unsigned int i;

    BUILD_BUG_ON(sizeof(struct infilfs_locator_disk) !=
                 INFILFS_LOCATOR_HEADER_BYTES);
    if (!block || !infilfs_locator_shape_valid(loc))
        return -EINVAL;

    memset(block, 0, INFILFS_DISK_BLOCK_SIZE);
    disk = (struct infilfs_locator_disk *)block;
    memcpy(disk->magic, infilfs_locator_magic, 8u);
    disk->schema = cpu_to_le16(INFILFS_LOCATOR_SCHEMA_VERSION);
    disk->header_size = cpu_to_le16(INFILFS_LOCATOR_HEADER_BYTES);
    disk->major = cpu_to_le16(INFILFS_FORMAT_MAJOR);
    disk->minor = cpu_to_le16(INFILFS_LOCATOR_FORMAT_MINOR);
    disk->generation = cpu_to_le64(loc->generation);
    disk->total_blocks = cpu_to_le64(loc->total_blocks);
    for (i = 0; i < INFILFS_CHECKPOINT_COUNT; ++i)
        disk->checkpoints[i] = cpu_to_le64(loc->checkpoint[i]);
    memcpy(disk->filesystem_uuid, loc->filesystem_uuid, 16u);
    checksum = cpu_to_le64(infilfs_rw_crc64_zeroed(
        block, INFILFS_DISK_BLOCK_SIZE,
        offsetof(struct infilfs_locator_disk, checksum),
        sizeof(disk->checksum)));
    memcpy(block + offsetof(struct infilfs_locator_disk, checksum),
           &checksum, sizeof(checksum));
    return 0;
}
