// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "infiltratorfs-windows-metadata.h"

#include <stdint.h>
#include <string.h>

int infilfs_windows_ticks_to_timestamp(INT64 ticks,
                                       struct infs_timestamp *out)
{
    const INT64 epoch = INT64_C(116444736000000000);
    INT64 delta;
    INT64 seconds;
    INT64 remainder;

    if (!out)
        return 0;
    delta = ticks - epoch;
    seconds = delta / INT64_C(10000000);
    remainder = delta % INT64_C(10000000);
    if (remainder < 0) {
        remainder += INT64_C(10000000);
        --seconds;
    }
    out->seconds = seconds;
    out->nanoseconds = (uint32_t)(remainder * 100);
    return 1;
}

INT64 infilfs_timestamp_to_windows_ticks(const struct infs_timestamp *value)
{
    const INT64 epoch_seconds = INT64_C(11644473600);
    if (!infs_timestamp_valid(value) || value->seconds < -epoch_seconds)
        return 0;
    if (value->seconds >
        INT64_MAX / INT64_C(10000000) - epoch_seconds)
        return INT64_MAX;
    return (value->seconds + epoch_seconds) * INT64_C(10000000) +
           (INT64)(value->nanoseconds / 100u);
}

uint64_t infilfs_windows_attributes_to_portable(DWORD attributes)
{
    uint64_t flags = 0;
    if (attributes & FILE_ATTRIBUTE_READONLY)
        flags |= INFS_ATTR_READ_ONLY;
    if (attributes & FILE_ATTRIBUTE_HIDDEN)
        flags |= INFS_ATTR_HIDDEN;
    if (attributes & FILE_ATTRIBUTE_SYSTEM)
        flags |= INFS_ATTR_SYSTEM;
    if (attributes & FILE_ATTRIBUTE_ARCHIVE)
        flags |= INFS_ATTR_ARCHIVE;
    if (attributes & FILE_ATTRIBUTE_TEMPORARY)
        flags |= INFS_ATTR_TEMPORARY;
    if (attributes & FILE_ATTRIBUTE_NOT_CONTENT_INDEXED)
        flags |= INFS_ATTR_NOT_CONTENT_INDEXED;
    return flags;
}

DWORD infilfs_portable_to_windows_attributes(uint64_t flags, int directory)
{
    DWORD attributes = directory ? FILE_ATTRIBUTE_DIRECTORY : 0;
    if (flags & INFS_ATTR_READ_ONLY)
        attributes |= FILE_ATTRIBUTE_READONLY;
    if (flags & INFS_ATTR_HIDDEN)
        attributes |= FILE_ATTRIBUTE_HIDDEN;
    if (flags & INFS_ATTR_SYSTEM)
        attributes |= FILE_ATTRIBUTE_SYSTEM;
    if (flags & INFS_ATTR_ARCHIVE)
        attributes |= FILE_ATTRIBUTE_ARCHIVE;
    if (flags & INFS_ATTR_TEMPORARY)
        attributes |= FILE_ATTRIBUTE_TEMPORARY;
    if (flags & INFS_ATTR_NOT_CONTENT_INDEXED)
        attributes |= FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    if (!attributes && !directory)
        attributes = FILE_ATTRIBUTE_NORMAL;
    return attributes;
}

void infilfs_windows_basic_to_time_update(
    const FILE_BASIC_INFO *basic, struct infs_time_update *update)
{
    if (!basic || !update)
        return;
    memset(update, 0, sizeof(*update));
    update->birth_action = INFS_TIME_SET;
    update->access_action = INFS_TIME_SET;
    update->modification_action = INFS_TIME_SET;
    update->change_action = INFS_TIME_SET;
    (void)infilfs_windows_ticks_to_timestamp(
        basic->CreationTime.QuadPart, &update->birth_time);
    (void)infilfs_windows_ticks_to_timestamp(
        basic->LastAccessTime.QuadPart, &update->access_time);
    (void)infilfs_windows_ticks_to_timestamp(
        basic->LastWriteTime.QuadPart, &update->modification_time);
    (void)infilfs_windows_ticks_to_timestamp(
        basic->ChangeTime.QuadPart, &update->change_time);
}
#endif
