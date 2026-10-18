// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/locator.h"

#include "infilfs/checksum.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "locator-format-019: %s\n", message);
        exit(1);
    }
}

static void put_u64(uint8_t *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i)
        p[i] = (uint8_t)(value >> (i * 8u));
}

static void fix_crc(uint8_t page[INFS_BLOCK_SIZE])
{
    memset(page + INFS_LOCATOR_OFF_CRC64, 0, 8);
    put_u64(page + INFS_LOCATOR_OFF_CRC64,
            infs_crc64_ecma(page, INFS_BLOCK_SIZE));
}

int main(void)
{
    struct infs_locator locator = {
        .generation = 17,
        .total_blocks = 32768,
        .checkpoint = {0, 8192, 32767},
        .filesystem_uuid = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}
    };
    struct infs_locator out = {0};
    uint8_t raw[INFS_BLOCK_SIZE];
    uint8_t bad[INFS_BLOCK_SIZE];

    expect(infs_locator_encode(raw, &locator) == INFS_STATUS_OK,
           "encode canonical locator");
    expect(infs_locator_decode(raw, 32768, &out) == INFS_STATUS_OK,
           "round-trip");
    expect(out.generation == locator.generation &&
           out.total_blocks == locator.total_blocks &&
           !memcmp(out.checkpoint, locator.checkpoint, sizeof(out.checkpoint)) &&
           !memcmp(out.filesystem_uuid, locator.filesystem_uuid, 16),
           "round-trip values");

    /* A current 0.18 implementation must not accept the future locator as a
     * checkpoint or confuse locator schema with a committed generation. */
    expect(INFS_FORMAT_MINOR == 19u,
           "current production disk format is 0.19");

    memcpy(bad, raw, sizeof(bad));
    bad[INFS_LOCATOR_OFF_GENERATION] ^= 1u;
    expect(infs_locator_decode(bad, 32768, &out) == INFS_STATUS_CORRUPT,
           "reject data corruption");
    memcpy(bad, raw, sizeof(bad));
    bad[INFS_LOCATOR_OFF_MINOR] = 18;
    fix_crc(bad);
    expect(infs_locator_decode(bad, 32768, &out) == INFS_STATUS_CORRUPT,
           "reject old format even with valid checksum");
    memcpy(bad, raw, sizeof(bad));
    bad[INFS_LOCATOR_HEADER_BYTES + 10] = 3;
    fix_crc(bad);
    expect(infs_locator_decode(bad, 32768, &out) == INFS_STATUS_CORRUPT,
           "reject noncanonical reserved bytes");
    memcpy(bad, raw, sizeof(bad));
    put_u64(bad + INFS_LOCATOR_OFF_CHECKPOINTS + 8, INFS_LOCATOR_BLOCK_B);
    fix_crc(bad);
    expect(infs_locator_decode(bad, 32768, &out) == INFS_STATUS_CORRUPT,
           "reject checkpoint aliasing a recovery locator");
    memcpy(bad, raw, sizeof(bad));
    put_u64(bad + INFS_LOCATOR_OFF_CHECKPOINTS + 16, 32768);
    fix_crc(bad);
    expect(infs_locator_decode(bad, 32768, &out) == INFS_STATUS_CORRUPT,
           "reject checkpoint outside committed geometry");
    expect(infs_locator_decode(raw, 16384, &out) == INFS_STATUS_CORRUPT,
           "reject claimed geometry larger than backing media");

    /* A stale but internally valid locator is only a candidate-source hint.
     * Recovery must separately prove the referenced committed graph. */
    locator.generation = 16;
    locator.checkpoint[1] = 8191;
    expect(infs_locator_encode(raw, &locator) == INFS_STATUS_OK,
           "a stale candidate map is valid input, not authority");
    expect(infs_locator_decode(raw, 32768, &out) == INFS_STATUS_OK &&
           out.generation == 16 && out.checkpoint[1] == 8191,
           "stale independent locator decode");

    locator.checkpoint[2] = locator.checkpoint[1];
    expect(infs_locator_encode(raw, &locator) == INFS_STATUS_INVALID_ARGUMENT,
           "reject duplicate checkpoint positions");

    {
        uint64_t root = 0;
        expect(infs_locator_plan_initial_metadata(4096, 64, &root) ==
               INFS_STATUS_OK && root == 2,
               "small 16 MiB volume boot metadata fits before locator B");
        expect(infs_locator_plan_initial_metadata(4096, 2046, &root) ==
               INFS_STATUS_NO_SPACE,
               "unplaceable bootstrap metadata must fail rather than collide");
        expect(infs_locator_plan_initial_metadata(UINT64_C(8000000), 2046,
                                                  &root) == INFS_STATUS_OK &&
               root == 2048,
               "large-volume bootstrap metadata starts after locator B");
        expect(infs_locator_plan_initial_metadata(4096, 1, &root) ==
               INFS_STATUS_OK && root == 2,
               "canonical metadata starting block is two");
    }

    {
        struct infs_locator copies[2] = {
            {
                .generation = 10,
                .total_blocks = 32768,
                .checkpoint = {0, 16384, 32767},
                .filesystem_uuid = {1}
            }, {
                .generation = 11,
                .total_blocks = 32768,
                .checkpoint = {0, 14000, 30000},
                .filesystem_uuid = {1}
            }
        };
        struct infs_locator_candidate list[INFS_LOCATOR_MAX_CANDIDATES];
        unsigned found = 0;
        expect(infs_locator_candidates(copies, 3, 32768, list, &found) ==
               INFS_STATUS_OK && found == 6,
               "preserve both old and new checkpoint maps during resize");
        expect(list[1].physical_block == 16384 &&
               list[4].physical_block == 14000 &&
               list[1].source_locator == 0 &&
               list[4].source_locator == 1,
               "do not select only the newer locator's metadata addresses");
        expect(infs_locator_candidates(copies, 2, 32768, list, &found) ==
               INFS_STATUS_OK && found == 3 &&
               list[0].source_locator == 1,
               "one damaged locator can be excluded independently");
        expect(infs_locator_candidates(copies, 0, 32768, list, &found) ==
               INFS_STATUS_CORRUPT,
               "both locators absent must fail without speculative scanning");
        expect(infs_locator_candidates(copies, 4, 32768, list, &found) ==
               INFS_STATUS_INVALID_ARGUMENT,
               "reject invalid locator source-mask");
        copies[1].filesystem_uuid[0] = 2;
        expect(infs_locator_candidates(copies, 3, 32768, list, &found) ==
               INFS_STATUS_CORRUPT,
               "reject mixed filesystem identities from both valid locators");
        copies[1].filesystem_uuid[0] = 1;
        copies[1].checkpoint[1] = INFS_LOCATOR_BLOCK_B;
        expect(infs_locator_candidates(copies, 3, 32768, list, &found) ==
               INFS_STATUS_CORRUPT,
               "reject locator map pointing to reserved locator position");
    }

    puts("locator-format-019: PASS");
    return 0;
}
