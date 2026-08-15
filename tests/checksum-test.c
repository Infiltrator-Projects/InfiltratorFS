// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/checksum.h"

#include <stdio.h>
#include <string.h>

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int check(const char *input, const char *expected)
{
    unsigned char digest[32];
    infs_sha256(input, strlen(input), digest);
    for (unsigned i = 0; i < 32; ++i) {
        int hi = hex_value(expected[i * 2u]);
        int lo = hex_value(expected[i * 2u + 1u]);
        if (hi < 0 || lo < 0 || digest[i] != (unsigned char)((hi << 4) | lo))
            return -1;
    }
    return 0;
}

int main(void)
{
    if (check("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") != 0 ||
        check("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0) {
        fputs("checksum-test: SHA-256 known-answer failure\n", stderr);
        return 1;
    }
    puts("checksum-test: PASS");
    return 0;
}
