// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/forensic.h"

#include "infilfs/endian.h"
#include "infilfs/fs.h"
#include "infilfs/volume.h"

#include <string.h>

struct borrowed_storage_context {
    const struct infs_storage *source;
};

static infs_status borrowed_read(void *opaque, uint64_t offset,
                                 void *buffer, size_t size)
{
    const struct borrowed_storage_context *context = opaque;
    return infs_storage_read(context->source, offset, buffer, size);
}

static infs_status borrowed_size(void *opaque, uint64_t *size_bytes,
                                 int *is_device)
{
    const struct borrowed_storage_context *context = opaque;
    int source_is_device = 0;
    infs_status status = infs_storage_get_size(
        context->source, size_bytes, &source_is_device);
    (void)source_is_device;
    /* Force the read-only opener through complete graph validation even when
     * the source is a block device. Raw classification must never trust an
     * allocation bitmap that has not passed full ownership validation. */
    *is_device = 0;
    return status;
}

static void borrowed_close(void *opaque)
{
    (void)opaque;
}

static const struct infs_storage_ops borrowed_ops = {
    .read_at = borrowed_read,
    .get_size = borrowed_size,
    .close = borrowed_close,
};

static int bitmap_get(const uint8_t *bitmap, uint64_t block)
{
    return (bitmap[block >> 3] >> (block & 7u)) & 1u;
}

static uint32_t classify_record(
    const struct infs_forensic_record *record,
    const struct infs_superblock_disk *current,
    const uint8_t *bitmap,
    int allocation_map_available,
    const uint8_t block[INFS_BLOCK_SIZE])
{
    if (record->kind == INFS_FORENSIC_CHECKPOINT) {
        if (!current)
            return INFS_FORENSIC_STATE_UNKNOWN;
        const struct infs_superblock_disk *candidate =
            (const struct infs_superblock_disk *)block;
        if (memcmp(candidate, current, sizeof(*current)) == 0)
            return INFS_FORENSIC_STATE_CURRENT;
        return INFS_FORENSIC_STATE_STALE;
    }
    if (!allocation_map_available)
        return INFS_FORENSIC_STATE_UNKNOWN;
    return bitmap_get(bitmap, record->block) ?
        INFS_FORENSIC_STATE_CURRENT : INFS_FORENSIC_STATE_ORPHANED;
}

static int decode_record(const uint8_t block[INFS_BLOCK_SIZE],
                         uint64_t block_no,
                         struct infs_forensic_record *record)
{
    memset(record, 0, sizeof(*record));
    record->block = block_no;

    if (infs_validate_superblock_block(block)) {
        const struct infs_superblock_disk *sb =
            (const struct infs_superblock_disk *)block;
        record->kind = INFS_FORENSIC_CHECKPOINT;
        record->generation = infs_le64_to_cpu(sb->generation);
        memcpy(record->object_id, sb->root_object_id, 16);
        return 1;
    }

    if (infs_validate_object_block(block)) {
        const struct infs_object_header_disk *header =
            (const struct infs_object_header_disk *)block;
        record->kind = INFS_FORENSIC_OBJECT;
        record->generation = infs_le64_to_cpu(header->generation);
        record->object_type = infs_le16_to_cpu(header->object_type);
        record->object_version = infs_le16_to_cpu(header->object_version);
        record->payload_size = infs_le32_to_cpu(header->payload_size);
        memcpy(record->object_id, header->object_id, 16);
        memcpy(record->parent_id, header->parent_id, 16);
        return 1;
    }

    const struct infs_metadata_page_disk *page =
        (const struct infs_metadata_page_disk *)block;
    const uint8_t *magic = NULL;
    uint32_t kind = 0;
    if (memcmp(page->magic, INFS_DIRECTORY_PAGE_MAGIC, 8) == 0) {
        magic = (const uint8_t *)INFS_DIRECTORY_PAGE_MAGIC;
        kind = INFS_FORENSIC_DIRECTORY_PAGE;
    } else if (memcmp(page->magic, INFS_INDEX_PAGE_MAGIC, 8) == 0) {
        magic = (const uint8_t *)INFS_INDEX_PAGE_MAGIC;
        kind = INFS_FORENSIC_INDEX_PAGE;
    }
    if (magic && infs_validate_metadata_page(
            block, magic, page->owner_object_id)) {
        record->kind = kind;
        record->generation = infs_le64_to_cpu(page->generation);
        record->entry_count = infs_le32_to_cpu(page->entry_count);
        record->bytes_used = infs_le32_to_cpu(page->bytes_used);
        memcpy(record->object_id, page->owner_object_id, 16);
        return 1;
    }
    return 0;
}

static void count_record(struct infs_forensic_summary *summary,
                         const struct infs_forensic_record *record)
{
    ++summary->records_found;
    switch (record->state) {
    case INFS_FORENSIC_STATE_CURRENT: ++summary->current_records; break;
    case INFS_FORENSIC_STATE_STALE: ++summary->stale_records; break;
    case INFS_FORENSIC_STATE_ORPHANED: ++summary->orphaned_records; break;
    default: ++summary->unknown_records; break;
    }
    switch (record->kind) {
    case INFS_FORENSIC_CHECKPOINT: ++summary->checkpoints_found; break;
    case INFS_FORENSIC_OBJECT: ++summary->objects_found; break;
    case INFS_FORENSIC_DIRECTORY_PAGE:
        ++summary->directory_pages_found;
        break;
    case INFS_FORENSIC_INDEX_PAGE: ++summary->index_pages_found; break;
    default: break;
    }
}

static int open_validated_current_graph(
    const struct infs_storage *storage,
    struct borrowed_storage_context *context,
    struct infs_volume *current,
    struct infs_forensic_summary *summary)
{
    context->source = storage;
    struct infs_storage borrowed = {
        .ops = &borrowed_ops,
        .context = context,
    };
    if (infs_volume_open_storage(current, &borrowed, 0) != INFS_STATUS_OK)
        return 0;
    summary->allocation_map_available = 1;
    summary->current_generation = infs_le64_to_cpu(current->sb.generation);
    return 1;
}

infs_status infs_forensic_scan(
    const struct infs_storage *storage,
    infs_forensic_record_callback callback,
    void *context,
    struct infs_forensic_summary *summary)
{
    if (!infs_storage_valid(storage) || !summary)
        return INFS_STATUS_INVALID_ARGUMENT;
    memset(summary, 0, sizeof(*summary));

    uint64_t size_bytes = 0;
    int is_device = 0;
    infs_status status = infs_storage_get_size(
        storage, &size_bytes, &is_device);
    (void)is_device;
    if (status != INFS_STATUS_OK)
        return status;
    summary->total_blocks = size_bytes / INFS_BLOCK_SIZE;
    if (summary->total_blocks == 0)
        return INFS_STATUS_INVALID_ARGUMENT;

    struct borrowed_storage_context borrowed_context;
    struct infs_volume current;
    const int current_open = open_validated_current_graph(
        storage, &borrowed_context, &current, summary);
    const struct infs_superblock_disk *current_sb =
        current_open ? &current.sb : NULL;
    const uint8_t *bitmap = current_open ? current.bitmap : NULL;

    uint8_t block[INFS_BLOCK_SIZE];
    for (uint64_t block_no = 0; block_no < summary->total_blocks;
         ++block_no) {
        status = infs_storage_read(storage, block_no * INFS_BLOCK_SIZE,
                                   block, sizeof(block));
        if (status != INFS_STATUS_OK) {
            if (current_open)
                infs_volume_close(&current);
            return status;
        }
        ++summary->scanned_blocks;

        struct infs_forensic_record record;
        if (!decode_record(block, block_no, &record))
            continue;
        record.state = classify_record(
            &record, current_sb, bitmap,
            summary->allocation_map_available, block);
        count_record(summary, &record);
        if (callback) {
            status = callback(&record, context);
            if (status != INFS_STATUS_OK) {
                if (current_open)
                    infs_volume_close(&current);
                return status;
            }
        }
    }
    if (current_open)
        infs_volume_close(&current);
    return INFS_STATUS_OK;
}
