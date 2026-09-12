// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_CHECK_H
#define INFILFS_CHECK_H

#include <stdint.h>

#include "infilfs/status.h"

struct infs_volume;

enum infs_check_stage {
    INFS_CHECK_STAGE_NONE = 0,
    INFS_CHECK_STAGE_CHECKPOINTS = 1,
    INFS_CHECK_STAGE_OBJECT_INDEX = 2,
    INFS_CHECK_STAGE_ALLOCATION_OWNERSHIP = 3,
    INFS_CHECK_STAGE_NAMESPACE = 4,
    INFS_CHECK_STAGE_CHECKSUM_METADATA = 5,
    INFS_CHECK_STAGE_COMPLETE = 6
};

struct infs_check_report {
    uint64_t check_generation;
    uint32_t checkpoint_replicas_valid;
    uint32_t failed_stage;
    int object_index_valid;
    int allocation_ownership_valid;
    int namespace_valid;
    int checksum_metadata_valid;
};

/*
 * Perform the normal fast filesystem consistency check. This validates the
 * filesystem's structural metadata and references but deliberately does not
 * read every file-data block or recompute payload checksums. Use infs_scrub()
 * for the explicit deep data-integrity operation.
 */
infs_status infs_check(struct infs_volume *vol,
                       struct infs_check_report *report);

#endif
