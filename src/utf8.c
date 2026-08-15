// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/utf8.h"

#include <stdint.h>

int infs_utf8_validate(const void *bytes, size_t length)
{
    const uint8_t *p = bytes;
    size_t i = 0;

    if (!p && length)
        return 0;

    while (i < length) {
        uint8_t first = p[i++];
        if (first <= 0x7f)
            continue;

        uint32_t value;
        unsigned continuation;
        uint32_t minimum;
        if (first >= 0xc2 && first <= 0xdf) {
            value = first & 0x1fu;
            continuation = 1;
            minimum = 0x80u;
        } else if (first >= 0xe0 && first <= 0xef) {
            value = first & 0x0fu;
            continuation = 2;
            minimum = 0x800u;
        } else if (first >= 0xf0 && first <= 0xf4) {
            value = first & 0x07u;
            continuation = 3;
            minimum = 0x10000u;
        } else {
            return 0;
        }

        if (continuation > length - i)
            return 0;
        for (unsigned n = 0; n < continuation; ++n) {
            uint8_t next = p[i++];
            if ((next & 0xc0u) != 0x80u)
                return 0;
            value = (value << 6) | (next & 0x3fu);
        }

        if (value < minimum || value > 0x10ffffu ||
            (value >= 0xd800u && value <= 0xdfffu))
            return 0;
    }

    return 1;
}
