// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_ENDIAN_H
#define INFILFS_ENDIAN_H

#include <stdint.h>

static inline uint16_t infs_bswap16(uint16_t value)
{
    return (uint16_t)((value >> 8) | (value << 8));
}

static inline uint32_t infs_bswap32(uint32_t value)
{
    return ((value & UINT32_C(0x000000ff)) << 24) |
           ((value & UINT32_C(0x0000ff00)) << 8) |
           ((value & UINT32_C(0x00ff0000)) >> 8) |
           ((value & UINT32_C(0xff000000)) >> 24);
}

static inline uint64_t infs_bswap64(uint64_t value)
{
    return ((uint64_t)infs_bswap32((uint32_t)value) << 32) |
           infs_bswap32((uint32_t)(value >> 32));
}

#if defined(_WIN32) || defined(__LITTLE_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define infs_cpu_to_le16(value) ((uint16_t)(value))
#define infs_cpu_to_le32(value) ((uint32_t)(value))
#define infs_cpu_to_le64(value) ((uint64_t)(value))
#define infs_le16_to_cpu(value) ((uint16_t)(value))
#define infs_le32_to_cpu(value) ((uint32_t)(value))
#define infs_le64_to_cpu(value) ((uint64_t)(value))
#else
#define infs_cpu_to_le16(value) infs_bswap16((uint16_t)(value))
#define infs_cpu_to_le32(value) infs_bswap32((uint32_t)(value))
#define infs_cpu_to_le64(value) infs_bswap64((uint64_t)(value))
#define infs_le16_to_cpu(value) infs_bswap16((uint16_t)(value))
#define infs_le32_to_cpu(value) infs_bswap32((uint32_t)(value))
#define infs_le64_to_cpu(value) infs_bswap64((uint64_t)(value))
#endif

#endif
