// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_NAMESPACE_POLICY_H
#define INFILFS_NAMESPACE_POLICY_H

/*
 * Format 0.19 namespace-policy v1 is a persistent byte contract shared by
 * portable code and the native Linux adapter. It must not acquire locale,
 * Unicode-table or OS-dependent behaviour without a new policy version.
 */
#ifdef __KERNEL__
#include <linux/string.h>
#include <linux/types.h>
#else
#include <stddef.h>
#include <string.h>
#endif

static inline unsigned char
infs_namespace_casefold_byte_v1(unsigned char value)
{
    return value >= 'A' && value <= 'Z' ?
        (unsigned char)(value + ('a' - 'A')) : value;
}

/*
 * Removable policy v1 excludes controls, Win32 punctuation, trailing dot/space
 * and reserved DOS basenames. Callers separately validate UTF-8 and traversal.
 */
static inline int
infs_namespace_removable_name_valid_v1(const unsigned char *name,
                                       size_t length)
{
    static const char *const reserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    char base[5] = {0};
    size_t base_length = 0;

    if (!name || !length || length > 255u ||
        name[length - 1u] == '.' || name[length - 1u] == ' ')
        return 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = name[i];
        if (c < 0x20u || c == 0x7fu ||
            c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
            return 0;
        if (c == '.')
            break;
        if (base_length < sizeof(base) - 1u) {
            if (c >= 'a' && c <= 'z')
                c = (unsigned char)(c - ('a' - 'A'));
            base[base_length++] = (char)c;
        } else {
            base_length = sizeof(base);
        }
    }
    if (base_length < sizeof(base)) {
        for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i)
            if (strlen(reserved[i]) == base_length &&
                memcmp(base, reserved[i], base_length) == 0)
                return 0;
    }
    return 1;
}
#endif /* INFILFS_NAMESPACE_POLICY_H */
