// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/namespace_policy.h"
#include <stdio.h>
#include <string.h>

static int expect_valid(const char *name, int expected)
{
    int actual = infs_namespace_removable_name_valid_v1(
        (const unsigned char *)name, strlen(name));
    if (actual == expected)
        return 0;
    fprintf(stderr, "namespace policy: '%s' unexpectedly %s\n",
            name, actual ? "accepted" : "rejected");
    return 1;
}

int main(void)
{
    static const struct {
        const char *name;
        int valid;
    } cases[] = {
        {"ordinary.txt", 1}, {"a.b.c", 1}, {"README", 1},
        {"COM0", 1}, {"COM10", 1}, {"COM10.dat", 1},
        {"CON", 0}, {"con.txt", 0}, {"pRn.log", 0},
        {"AUX", 0}, {"nul.bin", 0}, {"COM1", 0}, {"lpt9.dat", 0},
        {"trailing.", 0}, {"trailing ", 0}, {"path/name", 0},
        {"back\\slash", 0}, {"question?", 0}, {"star*", 0},
        {"colon:", 0}, {"angle<", 0}
    };
    unsigned char overlong[256];

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        if (expect_valid(cases[i].name, cases[i].valid))
            return 1;
    memset(overlong, 'a', sizeof(overlong));
    if (infs_namespace_removable_name_valid_v1(NULL, 3) ||
        infs_namespace_removable_name_valid_v1(overlong, 0) ||
        infs_namespace_removable_name_valid_v1(overlong, sizeof(overlong)) ||
        infs_namespace_removable_name_valid_v1(
            (const unsigned char *)"a\x7f", 2))
        return 1;
    for (unsigned int value = 0; value < 256u; ++value) {
        unsigned char folded = infs_namespace_casefold_byte_v1(
            (unsigned char)value);
        unsigned char expected = value >= 'A' && value <= 'Z' ?
            (unsigned char)(value + ('a' - 'A')) : (unsigned char)value;
        if (folded != expected) {
            fprintf(stderr, "namespace policy: wrong fold at %u\n", value);
            return 1;
        }
    }
    puts("Format 0.19 namespace byte policy passed.");
    return 0;
}
