// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATORFS_WINDOWS_BRIDGE_H
#define INFILTRATORFS_WINDOWS_BRIDGE_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#include "infilfs/volume.h"

/*
 * Driverless InfiltratorFS Windows bridge.
 *
 * The bridge uses Microsoft's inbox Projected File System (ProjFS) filter
 * driver and a user-mode provider. InfiltratorFS itself ships no Windows
 * kernel driver and therefore needs no InfiltratorFS driver signature for
 * this mode.
 *
 * State model:
 *
 * - The portable InfiltratorFS volume is the backing namespace and durability
 *   authority.
 * - A temporary NTFS virtualization root is the Windows/Explorer view. Files
 *   begin as lazy ProjFS placeholders and are hydrated from the portable core.
 * - Directories are materialized as ordinary NTFS directories before
 *   virtualization starts. This intentionally avoids ProjFS partial-directory
 *   rename restrictions and permits normal same-volume Explorer move-out.
 * - Provider identities associate callbacks with persistent InfiltratorFS
 *   objects; path aliases bridge rename notification ordering while Explorer
 *   and the backing namespace converge on the new name.
 * - Windows file contents are committed back after the completed close event,
 *   not on the earlier overwrite/pre-convert notification. The implementation
 *   compares local contents at filesystem-block granularity and writes only
 *   changed runs.
 * - Successful mutations are coalesced behind a short idle publication window;
 *   bridge shutdown forces the remaining publication before the volume leaves
 *   the provider.
 * - A completed move out of the virtualization root retires the corresponding
 *   backing tree only after Windows reports the move, so the bridge never
 *   deletes the source merely because a rename was attempted.
 *
 * Explorer access is rooted in the virtualization directory itself. An
 * auxiliary DOS drive alias is convenience only and may be unavailable across
 * UAC device namespaces without changing bridge correctness.
 */
struct infs_windows_bridge_stats {
    uint64_t files_imported;
    uint64_t bytes_examined;
    uint64_t bytes_written;
    uint64_t publish_count;
};

/* start borrows volume for the lifetime of the active bridge; the caller must
 * keep it open until stop returns. drive_out receives the optional DOS alias
 * when one is available and may otherwise be an empty string. */
int infs_windows_bridge_start(struct infs_volume *volume, HWND owner,
                              wchar_t *drive_out, size_t drive_out_count);

/* stop is the durability boundary for any mutation still waiting in the idle
 * coalescing window and releases all temporary ProjFS/NTFS bridge state. */
void infs_windows_bridge_stop(void);
int infs_windows_bridge_active(void);
int infs_windows_bridge_root(wchar_t *root_out, size_t root_out_count);
int infs_windows_bridge_get_stats(struct infs_windows_bridge_stats *stats);

#endif
#endif
