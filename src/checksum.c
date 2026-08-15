// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/checksum.h"

uint64_t infs_crc64_ecma(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t crc = 0;
    const uint64_t poly = UINT64_C(0x42F0E1EBA9EA3693);

    while (len--) {
        crc ^= (uint64_t)(*p++) << 56;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & UINT64_C(0x8000000000000000))
                    ? (crc << 1) ^ poly
                    : (crc << 1);
    }
    return crc;
}
