// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/endian.h"
#include "infilfs/format.h"
#include "infilfs/fs.h"
#include "infilfs/storage.h"
#include "infilfs/volume.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GEOMETRY_BLOCKS UINT64_C(32769)
#define GEOMETRY_SIZE (GEOMETRY_BLOCKS * (uint64_t)INFS_BLOCK_SIZE)

struct fake_storage {
    uint8_t checkpoint[INFS_BLOCK_SIZE];
    uint64_t size_bytes;
};

static void fail(const char *message)
{
    fprintf(stderr, "open-hardening: %s\n", message);
    exit(1);
}

static void expect(int condition, const char *message)
{
    if (!condition)
        fail(message);
}

static infs_status fake_read(void *context, uint64_t offset,
                             void *buffer, size_t size)
{
    struct fake_storage *fake = context;
    if (offset > fake->size_bytes || size > fake->size_bytes - offset)
        return INFS_STATUS_IO_ERROR;
    if (size == INFS_BLOCK_SIZE) {
        uint64_t block = offset / INFS_BLOCK_SIZE;
        if (block == 0 || block == GEOMETRY_BLOCKS / 2u ||
            block == GEOMETRY_BLOCKS - 1u) {
            memcpy(buffer, fake->checkpoint, INFS_BLOCK_SIZE);
            return INFS_STATUS_OK;
        }
    }
    memset(buffer, 0, size);
    return INFS_STATUS_OK;
}

static infs_status fake_size(void *context, uint64_t *size_bytes,
                             int *is_device)
{
    struct fake_storage *fake = context;
    *size_bytes = fake->size_bytes;
    *is_device = 0;
    return INFS_STATUS_OK;
}

static const struct infs_storage_ops read_only_ops = {
    .read_at = fake_read,
    .get_size = fake_size,
};

static void build_geometry_checkpoint(struct fake_storage *fake)
{
    static const uint8_t filesystem_id[16] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
    };
    static const uint8_t root_id[16] = {
        17, 18, 19, 20, 21, 22, 23, 24,
        25, 26, 27, 28, 29, 30, 31, 32
    };

    struct infs_superblock_disk sb;
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, INFS_MAGIC, 8);
    sb.format_major = infs_cpu_to_le16(INFS_FORMAT_MAJOR);
    sb.format_minor = infs_cpu_to_le16(INFS_FORMAT_MINOR);
    sb.header_size = infs_cpu_to_le16(sizeof(sb));
    sb.block_shift = infs_cpu_to_le16(INFS_BLOCK_SHIFT);
    sb.checksum_type = infs_cpu_to_le32(INFS_CHECKSUM_CRC64_ECMA);
    sb.generation = infs_cpu_to_le64(1);
    sb.total_blocks = infs_cpu_to_le64(GEOMETRY_BLOCKS);
    sb.free_blocks = infs_cpu_to_le64(0);
    sb.bitmap_start_block = infs_cpu_to_le64(1);
    /* One bitmap block covers exactly 32768 blocks, one fewer than required. */
    sb.bitmap_block_count = infs_cpu_to_le64(1);
    sb.object_index_block = infs_cpu_to_le64(2);
    sb.root_object_block = infs_cpu_to_le64(3);
    sb.checkpoint_block[0] = infs_cpu_to_le64(0);
    sb.checkpoint_block[1] = infs_cpu_to_le64(GEOMETRY_BLOCKS / 2u);
    sb.checkpoint_block[2] = infs_cpu_to_le64(GEOMETRY_BLOCKS - 1u);
    memcpy(sb.filesystem_uuid, filesystem_id, sizeof(filesystem_id));
    memcpy(sb.root_object_id, root_id, sizeof(root_id));
    sb.incompat_flags = infs_cpu_to_le64(
        INFS_INCOMPAT_UTF8_NAMES | INFS_INCOMPAT_SPARSE_EXTENTS);
    memcpy(sb.label, "Geometry", 8);

    expect(infs_encode_superblock(fake->checkpoint, &sb) == INFS_STATUS_OK,
           "encode geometry checkpoint");
}

static void check_undersized_bitmap(void)
{
    struct fake_storage fake;
    memset(&fake, 0, sizeof(fake));
    fake.size_bytes = GEOMETRY_SIZE;
    build_geometry_checkpoint(&fake);

    struct infs_storage storage = {
        .ops = &read_only_ops,
        .context = &fake,
    };
    struct infs_volume volume;
    infs_status status = infs_volume_open_storage(&volume, &storage, 0);
    expect(status == INFS_STATUS_CORRUPT,
           "reject bitmap too small for total_blocks before bit traversal");
}

static void check_writable_backend_requirements(void)
{
    struct fake_storage fake;
    memset(&fake, 0, sizeof(fake));
    fake.size_bytes = GEOMETRY_SIZE;

    struct infs_storage storage = {
        .ops = &read_only_ops,
        .context = &fake,
    };
    struct infs_volume volume;
    infs_status status = infs_volume_open_storage(&volume, &storage, 1);
    expect(status == INFS_STATUS_INVALID_ARGUMENT,
           "writable open requires write, flush, random and clock services");
}

int main(void)
{
    check_undersized_bitmap();
    check_writable_backend_requirements();
    puts("open-hardening: PASS");
    return 0;
}
