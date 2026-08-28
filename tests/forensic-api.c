// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/forensic.h"

#include "infilfs/endian.h"
#include "infilfs/fs.h"

#include <stdio.h>
#include <string.h>

#define TEST_BLOCKS 8u

struct memory_storage {
    uint8_t bytes[TEST_BLOCKS * INFS_BLOCK_SIZE];
};

struct observed {
    uint64_t current;
    uint64_t orphaned;
    uint64_t unknown;
};

static infs_status memory_read(void *opaque, uint64_t offset,
                               void *buffer, size_t size)
{
    struct memory_storage *memory = opaque;
    if (offset > sizeof(memory->bytes) ||
        size > sizeof(memory->bytes) - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
    memcpy(buffer, memory->bytes + (size_t)offset, size);
    return INFS_STATUS_OK;
}

static infs_status memory_size(void *opaque, uint64_t *size_bytes,
                               int *is_device)
{
    struct memory_storage *memory = opaque;
    *size_bytes = sizeof(memory->bytes);
    *is_device = 0;
    return INFS_STATUS_OK;
}

static const struct infs_storage_ops memory_ops = {
    .read_at = memory_read,
    .get_size = memory_size,
};

static infs_status observe_record(const struct infs_forensic_record *record,
                                  void *opaque)
{
    struct observed *observed = opaque;
    if (record->state == INFS_FORENSIC_STATE_CURRENT)
        ++observed->current;
    else if (record->state == INFS_FORENSIC_STATE_ORPHANED)
        ++observed->orphaned;
    else if (record->state == INFS_FORENSIC_STATE_UNKNOWN)
        ++observed->unknown;
    return INFS_STATUS_OK;
}

static int fail(const char *message)
{
    fprintf(stderr, "forensic-api: %s\n", message);
    return 1;
}

int main(void)
{
    struct memory_storage memory;
    memset(&memory, 0, sizeof(memory));
    struct infs_storage storage = {
        .ops = &memory_ops,
        .context = &memory,
    };

    const uint8_t filesystem_id[16] = { 0x11 };
    const uint8_t root_id[16] = { 0x22 };
    const uint8_t index_id[16] = { 0x33 };
    struct infs_superblock_disk sb;
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, INFS_MAGIC, 8);
    sb.format_major = infs_cpu_to_le16(INFS_FORMAT_MAJOR);
    sb.format_minor = infs_cpu_to_le16(INFS_FORMAT_MINOR);
    sb.header_size = infs_cpu_to_le16(sizeof(sb));
    sb.block_shift = infs_cpu_to_le16(INFS_BLOCK_SHIFT);
    sb.checksum_type = infs_cpu_to_le32(INFS_CHECKSUM_CRC64_ECMA);
    sb.generation = infs_cpu_to_le64(5);
    sb.total_blocks = infs_cpu_to_le64(TEST_BLOCKS);
    sb.free_blocks = infs_cpu_to_le64(2);
    sb.allocation_root_block = infs_cpu_to_le64(1);
    sb.allocation_leaf_count = infs_cpu_to_le64(1);
    sb.object_index_block = infs_cpu_to_le64(2);
    sb.root_object_block = infs_cpu_to_le64(3);
    sb.checkpoint_block[0] = infs_cpu_to_le64(0);
    sb.checkpoint_block[1] = infs_cpu_to_le64(4);
    sb.checkpoint_block[2] = infs_cpu_to_le64(7);
    memcpy(sb.filesystem_uuid, filesystem_id, 16);
    memcpy(sb.root_object_id, root_id, 16);
    sb.incompat_flags = infs_cpu_to_le64(
        INFS_INCOMPAT_UTF8_NAMES | INFS_INCOMPAT_SPARSE_EXTENTS |
        INFS_INCOMPAT_INLINE_DATA | INFS_INCOMPAT_SHARED_EXTENTS |
        INFS_INCOMPAT_PAGED_METADATA | INFS_INCOMPAT_ALLOCATION_TREE);
    memcpy(sb.label, "ForensicAPI", sizeof("ForensicAPI"));

    uint8_t checkpoint[INFS_BLOCK_SIZE];
    if (infs_encode_superblock(checkpoint, &sb) != INFS_STATUS_OK)
        return fail("could not encode checkpoint");
    memcpy(memory.bytes, checkpoint, INFS_BLOCK_SIZE);
    memcpy(memory.bytes + 4u * INFS_BLOCK_SIZE, checkpoint, INFS_BLOCK_SIZE);
    memcpy(memory.bytes + 7u * INFS_BLOCK_SIZE, checkpoint, INFS_BLOCK_SIZE);

    uint8_t *bitmap = memory.bytes + INFS_BLOCK_SIZE;
    bitmap[0] = UINT8_C(0x9f); /* current blocks 0,1,2,3,4 and 7 */

    if (infs_encode_paged_object_index(
            memory.bytes + 2u * INFS_BLOCK_SIZE, index_id, 5) !=
        INFS_STATUS_OK ||
        infs_encode_paged_root_directory(
            memory.bytes + 3u * INFS_BLOCK_SIZE, root_id, 5,
            0755, 0, 0, 1) != INFS_STATUS_OK)
        return fail("could not encode current objects");

    uint8_t *allocation = memory.bytes + 6u * INFS_BLOCK_SIZE;
    if (infs_allocation_page_init(
            allocation, INFS_ALLOCATION_LEAF_PAGE_MAGIC, 4u, 0u, 0u) !=
        INFS_STATUS_OK)
        return fail("could not initialize forensic allocation leaf");
    struct infs_allocation_page_disk *allocation_page =
        (struct infs_allocation_page_disk *)allocation;
    allocation_page->entry_count = infs_cpu_to_le32(TEST_BLOCKS);
    allocation_page->bytes_used = infs_cpu_to_le32(1u);
    ((uint8_t *)(allocation_page + 1))[0] = UINT8_C(0x9f);
    if (infs_allocation_page_finalize(allocation) != INFS_STATUS_OK)
        return fail("could not finalize forensic allocation leaf");

    uint8_t *orphan = memory.bytes + 5u * INFS_BLOCK_SIZE;
    if (infs_metadata_page_init(
            orphan, (const uint8_t *)INFS_INDEX_PAGE_MAGIC, index_id, 4) !=
        INFS_STATUS_OK || infs_metadata_page_finalize(orphan) !=
        INFS_STATUS_OK)
        return fail("could not encode orphaned page");

    struct observed observed = {0};
    struct infs_forensic_summary summary;
    if (infs_forensic_scan(&storage, observe_record, &observed, &summary) !=
        INFS_STATUS_OK)
        return fail("scan failed");
    if (summary.allocation_map_available || summary.current_generation != 0 ||
        summary.records_found != 7 ||
        summary.allocation_leaf_pages_found != 1 ||
        observed.current != 0 || observed.orphaned != 0 ||
        observed.unknown != 7)
        return fail("unverified graph was trusted for classification");

    memset(memory.bytes, 0, INFS_BLOCK_SIZE);
    memset(memory.bytes + 4u * INFS_BLOCK_SIZE, 0, INFS_BLOCK_SIZE);
    memset(memory.bytes + 7u * INFS_BLOCK_SIZE, 0, INFS_BLOCK_SIZE);
    memset(&observed, 0, sizeof(observed));
    if (infs_forensic_scan(&storage, observe_record, &observed, &summary) !=
        INFS_STATUS_OK)
        return fail("checkpointless scan failed");
    if (summary.allocation_map_available || summary.current_generation != 0 ||
        summary.records_found != 4 ||
        summary.allocation_leaf_pages_found != 1 ||
        observed.unknown != 4)
        return fail("checkpointless discovery mismatch");

    puts("forensic-api: PASS");
    return 0;
}
