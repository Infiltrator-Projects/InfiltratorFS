// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_WIN32_IO_H
#define INFILFS_WIN32_IO_H

#ifdef _WIN32
#include <wchar.h>
#include "infilfs/storage.h"

/* Open either a normal Windows file or a raw Windows volume such as
 * \\.\E:. For destructive/writable raw-volume work, set
 * lock_and_dismount nonzero so Windows is excluded from the volume first. */
infs_status infs_win32_storage_open(struct infs_storage *storage,
                                    const wchar_t *path, int writable,
                                    int lock_and_dismount);
#endif

#endif
