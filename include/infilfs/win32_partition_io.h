// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_WIN32_PARTITION_IO_H
#define INFILFS_WIN32_PARTITION_IO_H

#ifdef _WIN32
#include <stdint.h>
#include <wchar.h>

#include "infilfs/storage.h"

/* Open a bounded partition region on Windows. The function first tries the
 * supplied backing path directly (normally \\.\PhysicalDriveN). If that raw
 * disk handle cannot be opened for data access, it resolves the region back
 * to the matching NT partition device and opens that instead. The returned
 * storage view is always bounded to region_size bytes. */
infs_status infs_win32_storage_open_partition_region(
    struct infs_storage *storage,
    const wchar_t *path,
    uint64_t base_offset,
    uint64_t region_size,
    int writable);
#endif

#endif
