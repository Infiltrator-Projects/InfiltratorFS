// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_WIN32_IO_H
#define INFILFS_WIN32_IO_H

#ifdef _WIN32
#include <stdint.h>
#include <wchar.h>
#include "infilfs/storage.h"

/* Open either a normal Windows file or a raw Windows volume such as
 * \\.\E: or a volume-GUID path. For destructive/writable raw-volume work,
 * set lock_and_dismount nonzero so Windows is excluded from the volume first. */
infs_status infs_win32_storage_open(struct infs_storage *storage,
                                    const wchar_t *path, int writable,
                                    int lock_and_dismount);

/* Open a bounded byte range inside a larger Windows file/device, primarily a
 * partition discovered from IOCTL_DISK_GET_DRIVE_LAYOUT_EX on a
 * \\.\PhysicalDriveN handle. All storage offsets are translated by
 * base_offset and are strictly clipped to region_size so the generic core can
 * never read or write outside the selected partition. The caller must only use
 * writable regions that are not mounted by Windows. */
infs_status infs_win32_storage_open_region(struct infs_storage *storage,
                                           const wchar_t *path,
                                           uint64_t base_offset,
                                           uint64_t region_size,
                                           int writable);
#endif

#endif
