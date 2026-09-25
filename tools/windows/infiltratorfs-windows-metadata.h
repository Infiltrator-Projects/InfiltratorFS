// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATORFS_WINDOWS_METADATA_H
#define INFILTRATORFS_WINDOWS_METADATA_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "infilfs/volume.h"
#include "infilfs/win32_security.h"

int infilfs_windows_ticks_to_timestamp(INT64 ticks,
                                       struct infs_timestamp *out);
INT64 infilfs_timestamp_to_windows_ticks(
    const struct infs_timestamp *value);
uint64_t infilfs_windows_attributes_to_portable(DWORD attributes);
DWORD infilfs_portable_to_windows_attributes(uint64_t flags, int directory);
void infilfs_windows_basic_to_time_update(
    const FILE_BASIC_INFO *basic, struct infs_time_update *update);
infs_status infilfs_windows_import_security(
    struct infs_volume *volume, const char *path,
    const wchar_t *native_path, int directory);
infs_status infilfs_windows_export_security(
    struct infs_volume *volume, const char *path,
    const wchar_t *native_path, int directory);
#endif

#endif
