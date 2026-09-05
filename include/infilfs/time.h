// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_TIME_H
#define INFILFS_TIME_H

#include <stdint.h>

/*
 * Portable timestamp value used by Format 0.18 and the storage/API boundary.
 * Seconds are signed Unix-epoch seconds; nanoseconds are always 0..999999999.
 * Keeping the sub-second field separate avoids the signed 64-bit nanosecond
 * overflow around year 2262 while retaining nanosecond precision.
 */
struct infs_timestamp {
    int64_t seconds;
    uint32_t nanoseconds;
};

static inline int infs_timestamp_valid(const struct infs_timestamp *value)
{
    return value && value->nanoseconds < UINT32_C(1000000000);
}

#endif
