// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_LOCATOR_H
#define INFILFS_LOCATOR_H

#include "infilfs/format.h"
#include "infilfs/status.h"

#include <stdint.h>

/*
 * Format 0.19 discovery-only record. This API does not enable Format 0.19
 * mounting/formatting. The current shipping disk format remains 0.18 until
 * formatter, both recovery paths, fsck, and resize qualify together.
 *
 * The two positions are physical 4096-byte block offsets and do not move
 * during filesystem grow/shrink. The records are hints, NOT commit records.
 */
#define INFS_LOCATOR_FORMAT_MINOR 19u
#define INFS_LOCATOR_SCHEMA_VERSION 1u
#define INFS_LOCATOR_BLOCK_A UINT64_C(1)
#define INFS_LOCATOR_BLOCK_B UINT64_C(2047)
#define INFS_LOCATOR_MIN_VOLUME_BLOCKS UINT64_C(4096)
#define INFS_LOCATOR_HEADER_BYTES 80u

#define INFS_LOCATOR_OFF_MAGIC 0u
#define INFS_LOCATOR_OFF_SCHEMA 8u
#define INFS_LOCATOR_OFF_HEADER_SIZE 10u
#define INFS_LOCATOR_OFF_MAJOR 12u
#define INFS_LOCATOR_OFF_MINOR 14u
#define INFS_LOCATOR_OFF_GENERATION 16u
#define INFS_LOCATOR_OFF_TOTAL_BLOCKS 24u
#define INFS_LOCATOR_OFF_CHECKPOINTS 32u
#define INFS_LOCATOR_OFF_UUID 56u
#define INFS_LOCATOR_OFF_CRC64 72u

struct infs_locator {
    uint64_t generation;
    uint64_t total_blocks;
    uint64_t checkpoint[INFS_CHECKPOINT_COUNT];
    uint8_t filesystem_uuid[16];
};

/* Canonical, checksummed, little-endian 4096-byte encoding.
 * Decode checks internal fields against the physical device geometry only.
 * A caller MUST independently validate any referenced checkpoint and graph.
 */
infs_status infs_locator_encode(uint8_t block[INFS_BLOCK_SIZE],
                                const struct infs_locator *record);
infs_status infs_locator_decode(const uint8_t block[INFS_BLOCK_SIZE],
                                uint64_t physical_device_blocks,
                                struct infs_locator *out);

#endif
