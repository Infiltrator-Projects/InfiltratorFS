// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs-manager-open.h"

/*
 * Use the directory content-type handler, not generic URI dispatch.
 * xdg-open may route a native Linux path through an unrelated file-scheme
 * opener, notably an installed but unusable WSL bridge. Typed GIO dispatch
 * follows the session's registered file manager and reports launch errors.
 * Filesystem mounts and their lifetime remain outside this helper.
 */
gboolean infilfs_manager_open_directory(const char *path, GError **error)
{
    GAppInfo *handler;
    GFile *directory;
    GList files = {0};
    gboolean launched;

    if (!path || !g_path_is_absolute(path)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "A mounted directory needs an absolute path.");
        return FALSE;
    }
    if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                            "The mounted directory is not accessible.");
        return FALSE;
    }

    handler = g_app_info_get_default_for_type("inode/directory", FALSE);
    if (!handler) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "No file manager is registered for folders.");
        return FALSE;
    }

    directory = g_file_new_for_path(path);
    files.data = directory;
    launched = g_app_info_launch(handler, &files, NULL, error);
    g_object_unref(directory);
    g_object_unref(handler);
    return launched;
}
