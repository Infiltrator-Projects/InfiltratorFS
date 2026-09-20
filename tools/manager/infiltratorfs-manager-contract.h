// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATORFS_MANAGER_CONTRACT_H
#define INFILTRATORFS_MANAGER_CONTRACT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum infilfs_manager_action_id {
    INFILFS_MANAGER_ACTION_INSPECT = 0,
    INFILFS_MANAGER_ACTION_CHECK,
    INFILFS_MANAGER_ACTION_SCRUB,
    INFILFS_MANAGER_ACTION_FORENSIC,
    INFILFS_MANAGER_ACTION_COUNT
};

struct infilfs_manager_action_descriptor {
    enum infilfs_manager_action_id id;
    const char *title;
    const char *description;
    const char *button;
    const char *semantic_style;
    const char *linux_icon;
};

struct infilfs_manager_copy {
    const char *app_title;
    const char *subtitle;
    const char *storage_heading;
    const char *empty_title;
    const char *empty_copy;
    const char *overview_tab;
    const char *files_tab;
    const char *capacity_caption;
    const char *filesystem_caption;
    const char *status_caption;
    const char *volume_information;
    const char *device_type;
    const char *device_path;
    const char *volume_label;
    const char *mount_state;
    const char *mount_point;
    const char *maintenance;
    const char *danger_title;
    const char *danger_description;
    const char *format_button;
    const char *mount_button;
    const char *unmount_button;
    const char *new_image_button;
    const char *open_image_button;
    const char *refresh_button;
    const char *activity_heading;
    const char *clear_button;
};

const struct infilfs_manager_copy *infilfs_manager_copy(void);
const struct infilfs_manager_action_descriptor *infilfs_manager_action(
    enum infilfs_manager_action_id id);
size_t infilfs_manager_action_count(void);

#ifdef __cplusplus
}
#endif

#endif
