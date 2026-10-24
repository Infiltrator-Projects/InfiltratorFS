// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATORFS_MANAGER_OPEN_H
#define INFILTRATORFS_MANAGER_OPEN_H

#include <gio/gio.h>

/* Report whether desktop dispatch succeeded, not the external window's state. */
gboolean infilfs_manager_open_directory(const char *path, GError **error);

#endif
