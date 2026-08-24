// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_FORENSIC_H
#define INFILFS_FORENSIC_H

#include <stdint.h>

#include "infilfs/format.h"
#include "infilfs/storage.h"

#define INFS_FORENSIC_CHECKPOINT     UINT32_C(1)
#define INFS_FORENSIC_OBJECT         UINT32_C(2)
#define INFS_FORENSIC_DIRECTORY_PAGE UINT32_C(3)
#define INFS_FORENSIC_INDEX_PAGE     UINT32_C(4)

#define INFS_FORENSIC_STATE_UNKNOWN  UINT32_C(0)
#define INFS_FORENSIC_STATE_CURRENT  UINT32_C(1)
#define INFS_FORENSIC_STATE_STALE    UINT32_C(2)
#define INFS_FORENSIC_STATE_ORPHANED UINT32_C(3)

struct infs_forensic_record {
    uint64_t block;
    uint64_t generation;
    uint32_t kind;
    uint32_t state;
    uint16_t object_type;
    uint16_t object_version;
    uint32_t payload_size;
    uint32_t entry_count;
    uint32_t bytes_used;
    uint8_t object_id[16];
    uint8_t parent_id[16];
};

struct infs_forensic_summary {
    uint64_t total_blocks;
    uint64_t scanned_blocks;
    uint64_t records_found;
    uint64_t current_records;
    uint64_t stale_records;
    uint64_t orphaned_records;
    uint64_t unknown_records;
    uint64_t checkpoints_found;
    uint64_t objects_found;
    uint64_t directory_pages_found;
    uint64_t index_pages_found;
    uint64_t current_generation;
    int allocation_map_available;
};

typedef infs_status (*infs_forensic_record_callback)(
    const struct infs_forensic_record *record, void *context);

/* Scan every complete physical block without requiring the current namespace
 * graph to open. A valid current checkpoint and bitmap improve classification,
 * but their absence does not prevent raw record discovery. */
infs_status infs_forensic_scan(
    const struct infs_storage *storage,
    infs_forensic_record_callback callback,
    void *context,
    struct infs_forensic_summary *summary);

#endif
