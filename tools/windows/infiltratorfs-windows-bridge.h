// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATORFS_WINDOWS_BRIDGE_H
#define INFILTRATORFS_WINDOWS_BRIDGE_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>

#include "infilfs/volume.h"

/*
 * Driverless InfiltratorFS Windows bridge.
 *
 * The bridge uses Microsoft's inbox Projected File System (ProjFS) filter
 * driver and a user-mode provider. InfiltratorFS itself ships no Windows
 * kernel driver and therefore needs no InfiltratorFS driver signature for
 * this mode. A temporary NTFS virtualization root is projected into Explorer;
 * an auxiliary DOS drive alias is created when Windows permits it. File data is
 * hydrated directly from the portable InfiltratorFS core and Windows
 * modifications are committed back on close. Explorer access does not depend
 * on the DOS alias, which may live in a different UAC device namespace.
 */
int infs_windows_bridge_start(struct infs_volume *volume, HWND owner,
                              wchar_t *drive_out, size_t drive_out_count);
void infs_windows_bridge_stop(void);
int infs_windows_bridge_active(void);
int infs_windows_bridge_root(wchar_t *root_out, size_t root_out_count);

#endif
#endif
