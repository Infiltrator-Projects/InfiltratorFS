// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATORFS_WINDOWS_METADATA_H
#define INFILTRATORFS_WINDOWS_METADATA_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "infilfs/volume.h"

int infilfs_windows_ticks_to_timestamp(INT64 ticks,
                                       struct infs_timestamp *out);
INT64 infilfs_timestamp_to_windows_ticks(
    const struct infs_timestamp *value);
uint64_t infilfs_windows_attributes_to_portable(DWORD attributes);
DWORD infilfs_portable_to_windows_attributes(uint64_t flags, int directory);
void infilfs_windows_basic_to_time_update(
    const FILE_BASIC_INFO *basic, struct infs_time_update *update);
#endif

#endif
