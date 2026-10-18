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

/* A fixed upper bound, not a device-wide or metadata-tree search. Each of
 * two independently validated locator maps may identify three checkpoints. */
#define INFS_LOCATOR_MAX_CANDIDATES (2u * INFS_CHECKPOINT_COUNT)

struct infs_storage;

struct infs_locator_candidate {
    uint64_t physical_block;
    uint64_t declared_total_blocks;
    uint64_t declared_checkpoint[INFS_CHECKPOINT_COUNT];
    uint8_t filesystem_uuid[16];
    uint8_t source_locator;
};

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

/* Plan the canonical first allocation-tree block so the contiguous mkfs
 * bootstrap metadata cannot overlap either permanent 0.19 locator, the
 * midpoint checkpoint or the end checkpoint. metadata_blocks includes the
 * allocation tree, initial objects and initial index metadata. */
infs_status infs_locator_plan_initial_metadata(uint64_t total_blocks,
                                                 uint64_t metadata_blocks,
                                                 uint64_t *root_block);

/* Build a bounded candidate list from two previously validated locator
 * records. Both old and new checkpoint maps must be retained across resize.
 * No candidate here is authoritative until a full committed-graph check. */
infs_status infs_locator_candidates(
    const struct infs_locator locators[2], uint8_t valid_mask,
    uint64_t physical_blocks,
    struct infs_locator_candidate out[INFS_LOCATOR_MAX_CANDIDATES],
    unsigned *count);

/* Preliminary read-only Format 0.19 *checkpoint-header* discovery. This
 * examines both fixed locators and their six candidate positions, requires
 * exact locator/volume identity and canonical checkpoint CRC/shape, but
 * performs NO object-graph or allocation integrity validation. Callers MUST
 * NOT mount/write/heal from these headers without full graph qualification.
 * Normal production volume open deliberately does not call this function. */
infs_status infs_locator_discover_019_headers(
    const struct infs_storage *storage, uint64_t physical_size_bytes,
    struct infs_superblock_disk out[INFS_LOCATOR_MAX_CANDIDATES],
    uint8_t valid[INFS_LOCATOR_MAX_CANDIDATES],
    unsigned *valid_count);

#endif
