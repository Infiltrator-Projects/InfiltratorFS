// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATORFS_MANAGER_STORAGE_H
#define INFILTRATORFS_MANAGER_STORAGE_H

#include <glib.h>
#include <stdint.h>

/*
 * Linux Manager storage-discovery boundary.
 *
 * This model is intentionally separate from GTK widgets. Discovery excludes
 * the live system/root/boot device ancestry before returning format-capable
 * targets, and every block-device path is canonicalised before that safety
 * decision.  The UI owns presentation and user confirmation; this module owns
 * the platform-specific target inventory and observed mount state.
 */
typedef struct Target {
    char *path;
    char *name;
    char *media;
    char *filesystem;
    char *label;
    char *mountpoint;
    char *mount_fstype;
    uint64_t size;
    gboolean block;
} Target;

void target_free(gpointer data);
Target *target_new(const char *path, gboolean block);
void replace_string(char **slot, const char *value);
char *infiltratorfs_manager_canonical_path(const char *path);

gboolean spawn_capture(const char *const argv[], char **output,
                       char **error_output, int *wait_status);
gboolean command_success(const char *const argv[], char **output);

GPtrArray *discover_partitions(char **error_text);
char *mountpoint_for(const Target *target);
void refresh_mount(Target *target);

#endif
