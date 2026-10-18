// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/locator.h"

#include "infilfs/checksum.h"

#include <stddef.h>
#include <string.h>

static const uint8_t locator_magic[8] = {
    'I','N','F','S','L','0','1','9'
};

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= (uint64_t)p[i] << (i * 8u);
    return value;
}

static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_u64(uint8_t *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i)
        p[i] = (uint8_t)(value >> (i * 8u));
}

static int all_zero(const uint8_t *p, size_t bytes)
{
    uint8_t combined = 0;
    for (size_t i = 0; i < bytes; ++i)
        combined |= p[i];
    return combined == 0;
}

static int locator_fields_valid(const struct infs_locator *loc)
{
    if (!loc || !loc->generation ||
        loc->total_blocks < INFS_LOCATOR_MIN_VOLUME_BLOCKS ||
        all_zero(loc->filesystem_uuid, sizeof(loc->filesystem_uuid)) ||
        loc->checkpoint[0] != 0)
        return 0;

    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        const uint64_t pos = loc->checkpoint[i];
        if (pos >= loc->total_blocks ||
            pos == INFS_LOCATOR_BLOCK_A ||
            pos == INFS_LOCATOR_BLOCK_B)
            return 0;
        for (unsigned j = i + 1; j < INFS_CHECKPOINT_COUNT; ++j)
            if (pos == loc->checkpoint[j])
                return 0;
    }
    return 1;
}

infs_status infs_locator_encode(uint8_t block[INFS_BLOCK_SIZE],
                                const struct infs_locator *record)
{
    if (!block || !locator_fields_valid(record))
        return INFS_STATUS_INVALID_ARGUMENT;

    memset(block, 0, INFS_BLOCK_SIZE);
    memcpy(block + INFS_LOCATOR_OFF_MAGIC, locator_magic, sizeof(locator_magic));
    put_u16(block + INFS_LOCATOR_OFF_SCHEMA, INFS_LOCATOR_SCHEMA_VERSION);
    put_u16(block + INFS_LOCATOR_OFF_HEADER_SIZE, INFS_LOCATOR_HEADER_BYTES);
    put_u16(block + INFS_LOCATOR_OFF_MAJOR, INFS_FORMAT_MAJOR);
    put_u16(block + INFS_LOCATOR_OFF_MINOR, INFS_LOCATOR_FORMAT_MINOR);
    put_u64(block + INFS_LOCATOR_OFF_GENERATION, record->generation);
    put_u64(block + INFS_LOCATOR_OFF_TOTAL_BLOCKS, record->total_blocks);
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        put_u64(block + INFS_LOCATOR_OFF_CHECKPOINTS + i * 8u,
                record->checkpoint[i]);
    memcpy(block + INFS_LOCATOR_OFF_UUID, record->filesystem_uuid, 16);
    put_u64(block + INFS_LOCATOR_OFF_CRC64,
            infs_crc64_ecma(block, INFS_BLOCK_SIZE));
    return INFS_STATUS_OK;
}

infs_status infs_locator_decode(const uint8_t block[INFS_BLOCK_SIZE],
                                uint64_t physical_device_blocks,
                                struct infs_locator *out)
{
    struct infs_locator loc;
    uint8_t canonical[INFS_BLOCK_SIZE];

    if (!block || !out)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (physical_device_blocks < INFS_LOCATOR_MIN_VOLUME_BLOCKS ||
        memcmp(block + INFS_LOCATOR_OFF_MAGIC,
               locator_magic, sizeof(locator_magic)) != 0 ||
        get_u16(block + INFS_LOCATOR_OFF_SCHEMA) !=
            INFS_LOCATOR_SCHEMA_VERSION ||
        get_u16(block + INFS_LOCATOR_OFF_HEADER_SIZE) !=
            INFS_LOCATOR_HEADER_BYTES ||
        get_u16(block + INFS_LOCATOR_OFF_MAJOR) != INFS_FORMAT_MAJOR ||
        get_u16(block + INFS_LOCATOR_OFF_MINOR) !=
            INFS_LOCATOR_FORMAT_MINOR)
        return INFS_STATUS_CORRUPT;

    memcpy(canonical, block, sizeof(canonical));
    const uint64_t stored_crc = get_u64(canonical + INFS_LOCATOR_OFF_CRC64);
    memset(canonical + INFS_LOCATOR_OFF_CRC64, 0, 8);
    if (stored_crc != infs_crc64_ecma(canonical, sizeof(canonical)) ||
        !all_zero(block + INFS_LOCATOR_HEADER_BYTES,
                  INFS_BLOCK_SIZE - INFS_LOCATOR_HEADER_BYTES))
        return INFS_STATUS_CORRUPT;

    memset(&loc, 0, sizeof(loc));
    loc.generation = get_u64(block + INFS_LOCATOR_OFF_GENERATION);
    loc.total_blocks = get_u64(block + INFS_LOCATOR_OFF_TOTAL_BLOCKS);
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        loc.checkpoint[i] =
            get_u64(block + INFS_LOCATOR_OFF_CHECKPOINTS + i * 8u);
    memcpy(loc.filesystem_uuid, block + INFS_LOCATOR_OFF_UUID, 16);
    if (!locator_fields_valid(&loc) ||
        loc.total_blocks > physical_device_blocks)
        return INFS_STATUS_CORRUPT;

    *out = loc;
    return INFS_STATUS_OK;
}

infs_status infs_locator_plan_initial_metadata(uint64_t total_blocks,
                                                 uint64_t metadata_blocks,
                                                 uint64_t *root_block)
{
    uint64_t midpoint;
    if (!root_block || total_blocks < INFS_LOCATOR_MIN_VOLUME_BLOCKS ||
        !metadata_blocks || metadata_blocks > total_blocks)
        return INFS_STATUS_INVALID_ARGUMENT;
    midpoint = total_blocks / 2u;
    /* Half-open contiguous metadata extent starting at block two. */
    if (metadata_blocks <= INFS_LOCATOR_BLOCK_B - UINT64_C(2) &&
        metadata_blocks < midpoint - UINT64_C(2)) {
        *root_block = UINT64_C(2);
        return INFS_STATUS_OK;
    }
    /* Large initial allocation trees span the 8 MiB anchor: start beyond it
     * rather than allowing an extent to silently overwrite locator B. */
    if (midpoint > INFS_LOCATOR_BLOCK_B + UINT64_C(1) &&
        metadata_blocks < midpoint - (INFS_LOCATOR_BLOCK_B + UINT64_C(1))) {
        *root_block = INFS_LOCATOR_BLOCK_B + UINT64_C(1);
        return INFS_STATUS_OK;
    }
    return INFS_STATUS_NO_SPACE;
}

infs_status infs_locator_candidates(
    const struct infs_locator locators[2], uint8_t valid_mask,
    uint64_t physical_blocks,
    struct infs_locator_candidate out[INFS_LOCATOR_MAX_CANDIDATES],
    unsigned *count)
{
    unsigned used = 0;
    if (!locators || !out || !count || (valid_mask & (uint8_t)~3u))
        return INFS_STATUS_INVALID_ARGUMENT;
    *count = 0;
    for (unsigned i = 0; i < 2; ++i) {
        const struct infs_locator *loc = &locators[i];
        if (!(valid_mask & (1u << i)))
            continue;
        if (!locator_fields_valid(loc) ||
            loc->total_blocks > physical_blocks)
            return INFS_STATUS_CORRUPT;
        for (unsigned j = 0; j < INFS_CHECKPOINT_COUNT; ++j) {
            struct infs_locator_candidate *candidate = &out[used++];
            candidate->physical_block = loc->checkpoint[j];
            candidate->declared_total_blocks = loc->total_blocks;
            for (unsigned k = 0; k < INFS_CHECKPOINT_COUNT; ++k)
                candidate->declared_checkpoint[k] = loc->checkpoint[k];
            memcpy(candidate->filesystem_uuid,
                   loc->filesystem_uuid, sizeof(candidate->filesystem_uuid));
            candidate->source_locator = (uint8_t)i;
        }
    }
    if (!used)
        return INFS_STATUS_CORRUPT;
    *count = used;
    return INFS_STATUS_OK;
}
