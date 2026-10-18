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
    expect(INFS_FORMAT_MINOR == 18u,
           "current production disk format must remain unchanged");

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
    puts("locator-format-019: PASS");
    return 0;
}
