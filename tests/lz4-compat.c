// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/format.h"
#include <lz4.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint8_t input[8192];
    char encoded[LZ4_COMPRESSBOUND(8192)];
    char decoded[8192];
    int encoded_bytes;
    int decoded_bytes;
    size_t i;

    if (INFS_COMPRESSION_LZ4 != 1u || INFS_COMPRESSION_IAC1 != 2u)
        return 2;
    for (i = 0; i < sizeof(input); ++i)
        input[i] = (uint8_t)((i / 32u) ^ (i & 7u));
    encoded_bytes = LZ4_compress_default(
        (const char *)input, encoded, (int)sizeof(input),
        (int)sizeof(encoded));
    if (encoded_bytes <= 0)
        return 3;
    decoded_bytes = LZ4_decompress_safe(
        encoded, decoded, encoded_bytes, (int)sizeof(decoded));
    if (decoded_bytes != (int)sizeof(input) ||
        memcmp(input, decoded, sizeof(input)) != 0)
        return 4;
    puts("LZ4 retained codec compatibility: PASS");
    return 0;
}
