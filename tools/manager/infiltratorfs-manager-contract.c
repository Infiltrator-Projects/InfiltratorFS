// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs-manager-contract.h"

static const struct infilfs_manager_copy copy = {
    .app_title = "InfiltratorFS",
    .subtitle = "Filesystem management",
    .storage_heading = "STORAGE",
    .empty_title = "Select a storage target",
    .empty_copy = "Choose a partition, or open/create an InfiltratorFS image.",
    .overview_tab = "Overview",
    .files_tab = "Files",
    .capacity_caption = "CAPACITY",
    .filesystem_caption = "FILESYSTEM",
    .status_caption = "STATUS",
    .volume_information = "Volume information",
    .device_type = "Device type",
    .device_path = "Device path",
    .volume_label = "Volume label",
    .mount_state = "Mount state",
    .mount_point = "Mount point",
    .maintenance = "Maintenance",
    .danger_title = "Erase and format",
    .danger_description =
        "Permanently erase this target and create a new InfiltratorFS volume.",
    .format_button = "Format Volume...",
    .mount_button = "Mount and Open",
    .unmount_button = "Unmount",
    .new_image_button = "New Image",
    .open_image_button = "Open Image",
    .refresh_button = "Refresh",
    .activity_heading = "Activity log",
    .clear_button = "Clear",
};

static const struct infilfs_manager_action_descriptor actions[] = {
    {
        INFILFS_MANAGER_ACTION_INSPECT,
        "Inspect filesystem",
        "Read filesystem identity, format version and geometry.",
        "Inspect",
        "action-inspect",
        "document-properties-symbolic"
    },
    {
        INFILFS_MANAGER_ACTION_CHECK,
        "Check filesystem",
        "Run the fast structural consistency check.",
        "Check",
        "action-check",
        "emblem-ok-symbolic"
    },
    {
        INFILFS_MANAGER_ACTION_SCRUB,
        "Deep scrub",
        "Read payloads, recompute checksums and verify retained generations.",
        "Scrub",
        "action-scrub",
        "system-run-symbolic"
    },
    {
        INFILFS_MANAGER_ACTION_FORENSIC,
        "Forensic scan",
        "Locate current and orphaned filesystem metadata.",
        "Scan",
        "action-forensic",
        "system-search-symbolic"
    }
};

const struct infilfs_manager_copy *infilfs_manager_copy(void)
{
    return &copy;
}

const struct infilfs_manager_action_descriptor *infilfs_manager_action(
    enum infilfs_manager_action_id id)
{
    if ((unsigned)id >= (unsigned)INFILFS_MANAGER_ACTION_COUNT)
        return NULL;
    return &actions[(unsigned)id];
}

size_t infilfs_manager_action_count(void)
{
    return sizeof(actions) / sizeof(actions[0]);
}
