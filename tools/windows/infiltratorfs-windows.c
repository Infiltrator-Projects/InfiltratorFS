// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef _WIN32
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include "infilfs/endian.h"
#include "infilfs/format_volume.h"
#include "infilfs/format.h"
#include "infilfs/fs.h"
#include "infilfs/status.h"
#include "infilfs/volume.h"
#include "infilfs/win32_io.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef INFILFS_VERSION_W
#define INFILFS_VERSION_W L"0.9.6"
#endif

#define IDC_TARGET       1001
#define IDC_REFRESH      1002
#define IDC_LABEL        1003
#define IDC_FORMAT       1004
#define IDC_OPEN         1005
#define IDC_ADD_FILES    1006
#define IDC_ADD_FOLDER   1007
#define IDC_SCRUB        1008
#define IDC_CONTENTS     1009
#define IDC_STATUS       1010

#define IDM_FILE_REFRESH 2001
#define IDM_FILE_OPEN    2002
#define IDM_FILE_EXIT    2003
#define IDM_HELP_ABOUT   2101

#define MAX_TARGETS      256u
#define TARGET_PATH_MAX  160u
#define MAX_SYSTEM_DISKS 16u
#define MAX_PHYSICAL_DISKS 64u

struct target_volume {
    wchar_t device_path[TARGET_PATH_MAX];
    wchar_t volume_name[TARGET_PATH_MAX];
    wchar_t mount_point[MAX_PATH];
    uint64_t size_bytes;
    uint64_t region_offset;
    DWORD disk_number;
    DWORD partition_number;
    int have_disk_location;
    int use_region;
    int is_infiltrator;
    uint16_t format_major;
    uint16_t format_minor;
    wchar_t infs_label[INFS_LABEL_MAX + 1u];
};

static struct infs_volume g_volume;
static int g_volume_open = 0;
static HWND g_main_window = NULL;
static struct target_volume g_targets[MAX_TARGETS];
static size_t g_target_count = 0;
static DWORD g_system_disks[MAX_SYSTEM_DISKS];
static size_t g_system_disk_count = 0;

static void set_status(const wchar_t *text)
{
    if (!g_main_window)
        return;
    HWND control = GetDlgItem(g_main_window, IDC_STATUS);
    SetWindowTextW(control, text ? text : L"");
    UpdateWindow(control);
}

static void set_status_code(const wchar_t *action, infs_status status)
{
    wchar_t message[512];
    wchar_t detail[256] = L"unknown error";
    const char *ascii = infs_status_string(status);
    MultiByteToWideChar(CP_UTF8, 0, ascii, -1, detail,
                        (int)(sizeof(detail) / sizeof(detail[0])));
    _snwprintf_s(message, sizeof(message) / sizeof(message[0]), _TRUNCATE,
                 L"%s failed: %s (%d)", action, detail, (int)status);
    set_status(message);
    MessageBoxW(g_main_window, message, L"InfiltratorFS", MB_OK | MB_ICONERROR);
}

static void close_volume(void)
{
    if (g_volume_open) {
        infs_volume_close(&g_volume);
        memset(&g_volume, 0, sizeof(g_volume));
        g_volume_open = 0;
    }
}

static struct target_volume *selected_target(void)
{
    HWND combo = GetDlgItem(g_main_window, IDC_TARGET);
    LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR)
        return NULL;
    LRESULT data = SendMessageW(combo, CB_GETITEMDATA, (WPARAM)selected, 0);
    if (data == CB_ERR || data < 0 || (size_t)data >= g_target_count)
        return NULL;
    return &g_targets[(size_t)data];
}

static void update_buttons(void)
{
    int have_target = selected_target() != NULL;
    EnableWindow(GetDlgItem(g_main_window, IDC_FORMAT), have_target);
    EnableWindow(GetDlgItem(g_main_window, IDC_OPEN), have_target);
    EnableWindow(GetDlgItem(g_main_window, IDC_ADD_FILES), g_volume_open);
    EnableWindow(GetDlgItem(g_main_window, IDC_ADD_FOLDER), g_volume_open);
    EnableWindow(GetDlgItem(g_main_window, IDC_SCRUB), g_volume_open);
}

static void trim_volume_slash(const wchar_t *volume_name,
                              wchar_t out[TARGET_PATH_MAX])
{
    wcsncpy_s(out, TARGET_PATH_MAX, volume_name, _TRUNCATE);
    size_t length = wcslen(out);
    if (length && out[length - 1u] == L'\\')
        out[length - 1u] = L'\0';
}

static int first_mount_point(const wchar_t *volume_name,
                             wchar_t out[MAX_PATH])
{
    wchar_t paths[2048] = {0};
    DWORD needed = 0;
    if (!GetVolumePathNamesForVolumeNameW(volume_name, paths,
                                          (DWORD)(sizeof(paths) / sizeof(paths[0])),
                                          &needed) || !paths[0]) {
        out[0] = L'\0';
        return 0;
    }
    wcsncpy_s(out, MAX_PATH, paths, _TRUNCATE);
    return 1;
}

static int disk_is_system(DWORD disk_number)
{
    for (size_t i = 0; i < g_system_disk_count; ++i) {
        if (g_system_disks[i] == disk_number)
            return 1;
    }
    return 0;
}

static void remember_system_disk(DWORD disk_number)
{
    if (disk_is_system(disk_number) || g_system_disk_count >= MAX_SYSTEM_DISKS)
        return;
    g_system_disks[g_system_disk_count++] = disk_number;
}

static int query_volume_extents(const wchar_t *device_path,
                                VOLUME_DISK_EXTENTS **out_extents)
{
    if (!out_extents)
        return 0;
    *out_extents = NULL;
    HANDLE handle = CreateFileW(device_path, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;

    size_t capacity = sizeof(VOLUME_DISK_EXTENTS) + 15u * sizeof(DISK_EXTENT);
    for (unsigned attempt = 0; attempt < 8u; ++attempt) {
        if (capacity > UINT32_MAX) {
            CloseHandle(handle);
            return 0;
        }
        VOLUME_DISK_EXTENTS *extents = malloc(capacity);
        if (!extents) {
            CloseHandle(handle);
            return 0;
        }
        DWORD returned = 0;
        if (DeviceIoControl(handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                            NULL, 0, extents, (DWORD)capacity,
                            &returned, NULL)) {
            CloseHandle(handle);
            *out_extents = extents;
            return 1;
        }
        DWORD error = GetLastError();
        free(extents);
        if (error != ERROR_MORE_DATA)
            break;
        capacity *= 2u;
    }
    CloseHandle(handle);
    return 0;
}

static void discover_system_disks(void)
{
    g_system_disk_count = 0;
    wchar_t windows_dir[MAX_PATH] = {0};
    if (!GetWindowsDirectoryW(windows_dir, MAX_PATH) || !windows_dir[0])
        return;
    wchar_t device[8] = L"\\\\.\\C:";
    device[4] = windows_dir[0];
    VOLUME_DISK_EXTENTS *extents = NULL;
    if (!query_volume_extents(device, &extents))
        return;
    for (DWORD i = 0; i < extents->NumberOfDiskExtents; ++i)
        remember_system_disk(extents->Extents[i].DiskNumber);
    free(extents);
}

static int volume_is_system(const wchar_t *volume_name,
                            const wchar_t *system_volume)
{
    if (system_volume[0] && _wcsicmp(volume_name, system_volume) == 0)
        return 1;
    wchar_t device[TARGET_PATH_MAX];
    trim_volume_slash(volume_name, device);
    VOLUME_DISK_EXTENTS *extents = NULL;
    if (!query_volume_extents(device, &extents))
        return 0;
    int result = 0;
    for (DWORD i = 0; i < extents->NumberOfDiskExtents; ++i) {
        if (disk_is_system(extents->Extents[i].DiskNumber)) {
            result = 1;
            break;
        }
    }
    free(extents);
    return result;
}

static void superblock_label_to_wide(const struct infs_superblock_disk *sb,
                                     wchar_t out[INFS_LABEL_MAX + 1u])
{
    char label[INFS_LABEL_MAX + 1u];
    memcpy(label, sb->label, INFS_LABEL_MAX);
    label[INFS_LABEL_MAX] = '\0';
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, label, -1,
                             out, INFS_LABEL_MAX + 1u))
        wcscpy_s(out, INFS_LABEL_MAX + 1u, L"InfiltratorFS");
}

static int probe_infiltratorfs_volume(const wchar_t *device_path,
                                      uint64_t *size_bytes,
                                      struct infs_superblock_disk *sb)
{
    struct infs_storage storage = {0};
    infs_status status = infs_win32_storage_open(&storage, device_path, 0, 0);
    if (status != INFS_STATUS_OK)
        return 0;
    int is_device = 0;
    status = infs_storage_get_size(&storage, size_bytes, &is_device);
    if (status != INFS_STATUS_OK) {
        infs_storage_close(&storage);
        return 0;
    }
    (void)is_device;
    unsigned valid = 0;
    status = infs_read_best_superblock(&storage, *size_bytes, sb, &valid);
    infs_storage_close(&storage);
    return status == INFS_STATUS_OK && valid != 0;
}

static int probe_infiltratorfs_region(const wchar_t *device_path,
                                      uint64_t offset, uint64_t size_bytes,
                                      struct infs_superblock_disk *sb)
{
    struct infs_storage storage = {0};
    infs_status status = infs_win32_storage_open_region(
        &storage, device_path, offset, size_bytes, 0);
    if (status != INFS_STATUS_OK)
        return 0;
    unsigned valid = 0;
    status = infs_read_best_superblock(&storage, size_bytes, sb, &valid);
    infs_storage_close(&storage);
    return status == INFS_STATUS_OK && valid != 0;
}

static int add_combo_target(HWND combo, struct target_volume *candidate,
                            const wchar_t *display)
{
    if (g_target_count >= MAX_TARGETS)
        return 0;
    size_t target_index = g_target_count;
    g_targets[target_index] = *candidate;
    LRESULT combo_index = SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)display);
    if (combo_index == CB_ERR || combo_index == CB_ERRSPACE)
        return 0;
    SendMessageW(combo, CB_SETITEMDATA, (WPARAM)combo_index,
                 (LPARAM)target_index);
    ++g_target_count;
    return candidate->is_infiltrator ? 2 : 1;
}

static int add_volume_target(HWND combo, const wchar_t *volume_name,
                             const wchar_t *system_volume)
{
    if (g_target_count >= MAX_TARGETS ||
        volume_is_system(volume_name, system_volume))
        return 0;

    struct target_volume candidate;
    memset(&candidate, 0, sizeof(candidate));
    wcsncpy_s(candidate.volume_name, TARGET_PATH_MAX,
              volume_name, _TRUNCATE);
    trim_volume_slash(volume_name, candidate.device_path);
    int has_mount = first_mount_point(volume_name, candidate.mount_point);

    VOLUME_DISK_EXTENTS *extents = NULL;
    if (query_volume_extents(candidate.device_path, &extents)) {
        if (extents->NumberOfDiskExtents == 1u) {
            candidate.have_disk_location = 1;
            candidate.disk_number = extents->Extents[0].DiskNumber;
            candidate.region_offset =
                (uint64_t)extents->Extents[0].StartingOffset.QuadPart;
            candidate.size_bytes =
                (uint64_t)extents->Extents[0].ExtentLength.QuadPart;
        }
        free(extents);
    }

    struct infs_superblock_disk sb;
    memset(&sb, 0, sizeof(sb));
    uint64_t probed_size = 0;
    candidate.is_infiltrator = probe_infiltratorfs_volume(
        candidate.device_path, &probed_size, &sb);
    if (probed_size)
        candidate.size_bytes = probed_size;
    if (candidate.is_infiltrator) {
        candidate.format_major = infs_le16_to_cpu(sb.format_major);
        candidate.format_minor = infs_le16_to_cpu(sb.format_minor);
        superblock_label_to_wide(&sb, candidate.infs_label);
    }

    if (!candidate.size_bytes && has_mount) {
        ULARGE_INTEGER total = {0};
        if (GetDiskFreeSpaceExW(candidate.mount_point, NULL, &total, NULL))
            candidate.size_bytes = (uint64_t)total.QuadPart;
    }

    /* A zero-sized target cannot be bounded safely and is not useful for
     * formatting or probing. Do not show misleading 0.00 GiB ghost volumes. */
    if (!candidate.size_bytes)
        return 0;

    wchar_t windows_fs[64] = L"RAW / unknown";
    wchar_t windows_label[MAX_PATH] = L"";
    if (!candidate.is_infiltrator) {
        GetVolumeInformationW(volume_name, windows_label,
                              (DWORD)(sizeof(windows_label) / sizeof(windows_label[0])),
                              NULL, NULL, NULL, windows_fs,
                              (DWORD)(sizeof(windows_fs) / sizeof(windows_fs[0])));
    }

    wchar_t location[96];
    if (has_mount && wcslen(candidate.mount_point) >= 2u &&
        candidate.mount_point[1] == L':') {
        _snwprintf_s(location, sizeof(location) / sizeof(location[0]), _TRUNCATE,
                     L"%c:", candidate.mount_point[0]);
    } else if (has_mount) {
        wcsncpy_s(location, sizeof(location) / sizeof(location[0]),
                  candidate.mount_point, _TRUNCATE);
    } else if (candidate.have_disk_location) {
        _snwprintf_s(location, sizeof(location) / sizeof(location[0]), _TRUNCATE,
                     L"Disk %lu / no drive letter",
                     (unsigned long)candidate.disk_number);
    } else {
        wcscpy_s(location, sizeof(location) / sizeof(location[0]),
                  L"No drive letter");
    }

    wchar_t display[448];
    double gib = (double)candidate.size_bytes /
                 (1024.0 * 1024.0 * 1024.0);
    if (candidate.is_infiltrator) {
        _snwprintf_s(display, sizeof(display) / sizeof(display[0]), _TRUNCATE,
                     L"[InfiltratorFS %u.%u]  %s  %.2f GiB  %s",
                     (unsigned)candidate.format_major,
                     (unsigned)candidate.format_minor,
                     location, gib,
                     candidate.infs_label[0] ? candidate.infs_label : L"InfiltratorFS");
    } else {
        _snwprintf_s(display, sizeof(display) / sizeof(display[0]), _TRUNCATE,
                     L"%s  %s  %.2f GiB%s%s",
                     location, windows_fs, gib,
                     windows_label[0] ? L"  " : L"",
                     windows_label);
    }
    return add_combo_target(combo, &candidate, display);
}

static int target_matches_partition(DWORD disk_number,
                                    uint64_t offset, uint64_t size_bytes)
{
    for (size_t i = 0; i < g_target_count; ++i) {
        const struct target_volume *target = &g_targets[i];
        if (!target->have_disk_location)
            continue;
        if (target->disk_number == disk_number &&
            target->region_offset == offset &&
            target->size_bytes == size_bytes)
            return 1;
    }
    return 0;
}

static int physical_disk_is_removable(HANDLE disk)
{
    STORAGE_HOTPLUG_INFO hotplug;
    DWORD returned = 0;
    memset(&hotplug, 0, sizeof(hotplug));
    hotplug.Size = sizeof(hotplug);
    if (DeviceIoControl(disk, IOCTL_STORAGE_GET_HOTPLUG_INFO,
                        NULL, 0, &hotplug, sizeof(hotplug),
                        &returned, NULL) &&
        (hotplug.MediaRemovable || hotplug.DeviceHotplug))
        return 1;

    /* Built-in SD/MMC readers commonly report neither hot-plug flag even
     * though their media is removable. Ask the storage stack for the bus type
     * as a second source of truth. USB is included for external card readers. */
    STORAGE_PROPERTY_QUERY query;
    memset(&query, 0, sizeof(query));
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    uint8_t buffer[1024];
    memset(buffer, 0, sizeof(buffer));
    if (DeviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY,
                        &query, sizeof(query), buffer, sizeof(buffer),
                        &returned, NULL)) {
        STORAGE_DEVICE_DESCRIPTOR *descriptor =
            (STORAGE_DEVICE_DESCRIPTOR *)buffer;
        if (descriptor->BusType == BusTypeSd ||
            descriptor->BusType == BusTypeMmc ||
            descriptor->BusType == BusTypeUsb)
            return 1;
    }
    return 0;
}

static DRIVE_LAYOUT_INFORMATION_EX *read_drive_layout(HANDLE disk)
{
    size_t capacity = 64u * 1024u;
    for (unsigned attempt = 0; attempt < 6u; ++attempt) {
        if (capacity > UINT32_MAX)
            return NULL;
        DRIVE_LAYOUT_INFORMATION_EX *layout = malloc(capacity);
        if (!layout)
            return NULL;
        DWORD returned = 0;
        if (DeviceIoControl(disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                            NULL, 0, layout, (DWORD)capacity,
                            &returned, NULL))
            return layout;
        DWORD error = GetLastError();
        free(layout);
        if (error != ERROR_INSUFFICIENT_BUFFER && error != ERROR_MORE_DATA)
            return NULL;
        capacity *= 2u;
    }
    return NULL;
}

static int add_physical_partition_target(HWND combo, DWORD disk_number,
                                         const PARTITION_INFORMATION_EX *part,
                                         int removable)
{
    if (!part || part->PartitionNumber == 0 ||
        part->StartingOffset.QuadPart < 0 ||
        part->PartitionLength.QuadPart <= 0)
        return 0;

    uint64_t offset = (uint64_t)part->StartingOffset.QuadPart;
    uint64_t size_bytes = (uint64_t)part->PartitionLength.QuadPart;
    if (target_matches_partition(disk_number, offset, size_bytes))
        return 0;

    struct target_volume candidate;
    memset(&candidate, 0, sizeof(candidate));
    _snwprintf_s(candidate.device_path,
                 sizeof(candidate.device_path) / sizeof(candidate.device_path[0]),
                 _TRUNCATE, L"\\\\.\\PhysicalDrive%lu",
                 (unsigned long)disk_number);
    candidate.size_bytes = size_bytes;
    candidate.region_offset = offset;
    candidate.disk_number = disk_number;
    candidate.partition_number = part->PartitionNumber;
    candidate.have_disk_location = 1;
    candidate.use_region = 1;

    struct infs_superblock_disk sb;
    memset(&sb, 0, sizeof(sb));
    candidate.is_infiltrator = probe_infiltratorfs_region(
        candidate.device_path, offset, size_bytes, &sb);
    if (candidate.is_infiltrator) {
        candidate.format_major = infs_le16_to_cpu(sb.format_major);
        candidate.format_minor = infs_le16_to_cpu(sb.format_minor);
        superblock_label_to_wide(&sb, candidate.infs_label);
    }

    if (!candidate.is_infiltrator && !removable)
        return 0;

    wchar_t display[448];
    double gib = (double)size_bytes / (1024.0 * 1024.0 * 1024.0);
    if (candidate.is_infiltrator) {
        _snwprintf_s(display, sizeof(display) / sizeof(display[0]), _TRUNCATE,
                     L"[InfiltratorFS %u.%u]  Disk %lu partition %lu  %.2f GiB  %s",
                     (unsigned)candidate.format_major,
                     (unsigned)candidate.format_minor,
                     (unsigned long)disk_number,
                     (unsigned long)part->PartitionNumber,
                     gib,
                     candidate.infs_label[0] ? candidate.infs_label : L"InfiltratorFS");
    } else {
        _snwprintf_s(display, sizeof(display) / sizeof(display[0]), _TRUNCATE,
                     L"[RAW removable partition]  Disk %lu partition %lu  %.2f GiB",
                     (unsigned long)disk_number,
                     (unsigned long)part->PartitionNumber,
                     gib);
    }
    return add_combo_target(combo, &candidate, display);
}

static void enumerate_physical_partitions(HWND combo,
                                          int *added,
                                          int *infiltrator_count)
{
    for (DWORD disk_number = 0; disk_number < MAX_PHYSICAL_DISKS; ++disk_number) {
        if (disk_is_system(disk_number))
            continue;
        wchar_t path[64];
        _snwprintf_s(path, sizeof(path) / sizeof(path[0]), _TRUNCATE,
                     L"\\\\.\\PhysicalDrive%lu", (unsigned long)disk_number);
        HANDLE disk = CreateFileW(path, GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (disk == INVALID_HANDLE_VALUE)
            continue;
        int removable = physical_disk_is_removable(disk);
        DRIVE_LAYOUT_INFORMATION_EX *layout = read_drive_layout(disk);
        CloseHandle(disk);
        if (!layout)
            continue;

        for (DWORD i = 0; i < layout->PartitionCount; ++i) {
            int result = add_physical_partition_target(
                combo, disk_number, &layout->PartitionEntry[i], removable);
            if (result) {
                ++*added;
                if (result == 2)
                    ++*infiltrator_count;
            }
        }
        free(layout);
    }
}

static void refresh_volumes(void)
{
    HWND combo = GetDlgItem(g_main_window, IDC_TARGET);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    g_target_count = 0;
    discover_system_disks();

    wchar_t system_volume[TARGET_PATH_MAX] = L"";
    wchar_t windows_dir[MAX_PATH] = {0};
    if (GetWindowsDirectoryW(windows_dir, MAX_PATH) && windows_dir[0]) {
        wchar_t system_root[4] = {windows_dir[0], L':', L'\\', L'\0'};
        GetVolumeNameForVolumeMountPointW(system_root, system_volume,
                                          (DWORD)(sizeof(system_volume) /
                                                  sizeof(system_volume[0])));
    }

    int added = 0;
    int infiltrator_count = 0;
    wchar_t volume_name[TARGET_PATH_MAX];
    HANDLE find = FindFirstVolumeW(volume_name,
                                   (DWORD)(sizeof(volume_name) /
                                           sizeof(volume_name[0])));
    if (find != INVALID_HANDLE_VALUE) {
        for (;;) {
            int result = add_volume_target(combo, volume_name, system_volume);
            if (result) {
                ++added;
                if (result == 2)
                    ++infiltrator_count;
            }
            if (!FindNextVolumeW(find, volume_name,
                                 (DWORD)(sizeof(volume_name) /
                                         sizeof(volume_name[0]))))
                break;
        }
        FindVolumeClose(find);
    }

    enumerate_physical_partitions(combo, &added, &infiltrator_count);

    if (added)
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
    update_buttons();

    if (infiltrator_count > 0) {
        wchar_t text[224];
        _snwprintf_s(text, sizeof(text) / sizeof(text[0]), _TRUNCATE,
                     L"Found %d InfiltratorFS partition%s. Drive letters and Windows filesystem drivers are not required.",
                     infiltrator_count, infiltrator_count == 1 ? L"" : L"s");
        set_status(text);
    } else if (added) {
        set_status(L"No InfiltratorFS partition detected. Non-system Windows volumes and removable SD/MMC/USB partitions are shown.");
    } else {
        set_status(L"No usable non-system volumes or partitions were found.");
    }
}

static int utf8_component(const wchar_t *wide, char out[INFS_NAME_MAX + 1u])
{
    int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                                     NULL, 0, NULL, NULL);
    if (needed <= 0 || needed - 1 > (int)INFS_NAME_MAX)
        return 0;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                             out, INFS_NAME_MAX + 1u, NULL, NULL))
        return 0;
    if (strchr(out, '/'))
        return 0;
    return 1;
}

static const wchar_t *host_basename(const wchar_t *path)
{
    const wchar_t *slash = wcsrchr(path, L'\\');
    const wchar_t *forward = wcsrchr(path, L'/');
    if (!slash || (forward && forward > slash))
        slash = forward;
    return slash ? slash + 1 : path;
}

static int make_child_path(const char *parent, const char *name,
                           char out[INFS_PATH_MAX + 1u])
{
    int written;
    if (strcmp(parent, "/") == 0)
        written = snprintf(out, INFS_PATH_MAX + 1u, "/%s", name);
    else
        written = snprintf(out, INFS_PATH_MAX + 1u, "%s/%s", parent, name);
    return written > 0 && written <= (int)INFS_PATH_MAX;
}

static int copy_host_file(const wchar_t *host_path, const char *dest_path)
{
    HANDLE input = CreateFileW(host_path, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (input == INVALID_HANDLE_VALUE)
        return 0;

    struct infs_create_options options;
    memset(&options, 0, sizeof(options));
    options.posix_permissions = 0644u;
    infs_status status = infs_create_file(&g_volume, dest_path, &options);
    if (status == INFS_STATUS_ALREADY_EXISTS)
        status = infs_truncate_file(&g_volume, dest_path, 0u);
    if (status != INFS_STATUS_OK) {
        CloseHandle(input);
        set_status_code(L"Create destination file", status);
        return 0;
    }

    const DWORD chunk_size = 4u * 1024u * 1024u;
    uint8_t *buffer = malloc(chunk_size);
    if (!buffer) {
        CloseHandle(input);
        set_status_code(L"Allocate copy buffer", INFS_STATUS_NO_MEMORY);
        return 0;
    }
    uint64_t offset = 0;
    int okay = 1;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(input, buffer, chunk_size, &got, NULL)) {
            okay = 0;
            break;
        }
        if (!got)
            break;
        int64_t written = infs_write_file_buffered(
            &g_volume, dest_path, buffer, got, offset);
        if (written != (int64_t)got) {
            set_status_code(L"Write InfiltratorFS file",
                            written < 0 ? (infs_status)written : INFS_STATUS_IO_ERROR);
            okay = 0;
            break;
        }
        offset += got;
    }
    free(buffer);
    CloseHandle(input);
    if (okay) {
        status = infs_volume_sync(&g_volume);
        if (status != INFS_STATUS_OK) {
            set_status_code(L"Commit copied file", status);
            okay = 0;
        }
    }
    return okay;
}

static int copy_host_path(const wchar_t *host_path, const char *parent_dest)
{
    DWORD attrs = GetFileAttributesW(host_path);
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return 0;
    const wchar_t *base = host_basename(host_path);
    if (!base[0])
        return 0;
    char name[INFS_NAME_MAX + 1u];
    if (!utf8_component(base, name)) {
        MessageBoxW(g_main_window,
                    L"A filename is too long or cannot be represented as UTF-8.",
                    L"InfiltratorFS", MB_OK | MB_ICONERROR);
        return 0;
    }
    char dest[INFS_PATH_MAX + 1u];
    if (!make_child_path(parent_dest, name, dest))
        return 0;

    wchar_t status_text[512];
    _snwprintf_s(status_text, sizeof(status_text) / sizeof(status_text[0]),
                 _TRUNCATE, L"Copying %s ...", base);
    set_status(status_text);

    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY))
        return copy_host_file(host_path, dest);

    struct infs_create_options options;
    memset(&options, 0, sizeof(options));
    options.posix_permissions = 0755u;
    infs_status status = infs_mkdir(&g_volume, dest, &options);
    if (status != INFS_STATUS_OK && status != INFS_STATUS_ALREADY_EXISTS) {
        set_status_code(L"Create destination directory", status);
        return 0;
    }

    wchar_t pattern[32768];
    if (_snwprintf_s(pattern, sizeof(pattern) / sizeof(pattern[0]), _TRUNCATE,
                     L"%s\\*", host_path) < 0)
        return 0;
    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileW(pattern, &data);
    if (search == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    int okay = 1;
    do {
        if (wcscmp(data.cFileName, L".") == 0 ||
            wcscmp(data.cFileName, L"..") == 0)
            continue;
        wchar_t child[32768];
        if (_snwprintf_s(child, sizeof(child) / sizeof(child[0]), _TRUNCATE,
                         L"%s\\%s", host_path, data.cFileName) < 0 ||
            !copy_host_path(child, dest)) {
            okay = 0;
            break;
        }
    } while (FindNextFileW(search, &data));
    DWORD error = GetLastError();
    FindClose(search);
    if (okay && error != ERROR_NO_MORE_FILES)
        okay = 0;
    return okay;
}

static void refresh_contents(void)
{
    HWND list = GetDlgItem(g_main_window, IDC_CONTENTS);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    if (!g_volume_open)
        return;
    struct infs_dir_item *items = NULL;
    size_t count = 0;
    infs_status status = infs_list_dir(&g_volume, "/", &items, &count);
    if (status != INFS_STATUS_OK) {
        set_status_code(L"List volume", status);
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        wchar_t name[INFS_NAME_MAX + 1u];
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                 items[i].name, -1, name,
                                 (int)(sizeof(name) / sizeof(name[0]))))
            continue;
        wchar_t display[512];
        _snwprintf_s(display, sizeof(display) / sizeof(display[0]), _TRUNCATE,
                     items[i].type == INFS_OBJECT_DIRECTORY ?
                     L"[DIR]  %s" : L"       %s", name);
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)display);
    }
    infs_free_dir_items(items);
    wchar_t text[192];
    _snwprintf_s(text, sizeof(text) / sizeof(text[0]), _TRUNCATE,
                 L"Ready. %zu item%s in the root. Drag files or folders onto this window to copy them.",
                 count, count == 1u ? L"" : L"s");
    set_status(text);
}

static infs_status open_target_storage(struct target_volume *target,
                                       struct infs_storage *storage,
                                       int writable)
{
    if (target->use_region) {
        return infs_win32_storage_open_region(
            storage, target->device_path,
            target->region_offset, target->size_bytes, writable);
    }
    return infs_win32_storage_open(storage, target->device_path,
                                   writable, writable);
}

static int open_selected_volume(int format_first)
{
    struct target_volume *target = selected_target();
    if (!target) {
        MessageBoxW(g_main_window, L"Select a target volume or partition first.",
                    L"InfiltratorFS", MB_OK | MB_ICONWARNING);
        return 0;
    }

    if (format_first) {
        wchar_t prompt[896];
        if (target->use_region) {
            _snwprintf_s(prompt, sizeof(prompt) / sizeof(prompt[0]), _TRUNCATE,
                         L"FORMAT DISK %lu PARTITION %lu AS INFILTRATORFS?\n\nSize: %.2f GiB\n\nEverything currently in this partition will be destroyed. The raw storage view is bounded to this partition.",
                         (unsigned long)target->disk_number,
                         (unsigned long)target->partition_number,
                         (double)target->size_bytes /
                         (1024.0 * 1024.0 * 1024.0));
        } else {
            _snwprintf_s(prompt, sizeof(prompt) / sizeof(prompt[0]), _TRUNCATE,
                         L"FORMAT THIS VOLUME AS INFILTRATORFS?\n\nLocation: %s\nSize: %.2f GiB\n\nEverything currently on this volume will be destroyed. Windows will be locked out of the selected volume before writing.",
                         target->mount_point[0] ? target->mount_point : L"No drive letter",
                         (double)target->size_bytes /
                         (1024.0 * 1024.0 * 1024.0));
        }
        if (MessageBoxW(g_main_window, prompt, L"Confirm destructive format",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES)
            return 0;
    }

    close_volume();
    update_buttons();
    struct infs_storage storage = {0};
    set_status(target->use_region ?
               L"Opening bounded raw partition ..." :
               L"Locking selected Windows volume ...");
    infs_status status = open_target_storage(target, &storage, 1);
    if (status != INFS_STATUS_OK) {
        set_status_code(L"Open selected storage", status);
        return 0;
    }

    uint64_t detected_size = 0;
    int detected_device = 0;
    status = infs_storage_get_size(&storage, &detected_size, &detected_device);
    if (status != INFS_STATUS_OK) {
        infs_storage_close(&storage);
        set_status_code(L"Determine selected storage size", status);
        return 0;
    }
    (void)detected_device;

    if (format_first) {
        wchar_t wide_label[INFS_LABEL_MAX] = L"InfiltratorFS";
        GetWindowTextW(GetDlgItem(g_main_window, IDC_LABEL), wide_label,
                       (int)(sizeof(wide_label) / sizeof(wide_label[0])));
        char label[INFS_LABEL_MAX * 4u];
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_label, -1,
                                 label, (int)sizeof(label), NULL, NULL)) {
            infs_storage_close(&storage);
            MessageBoxW(g_main_window, L"The volume label is not valid Unicode.",
                        L"InfiltratorFS", MB_OK | MB_ICONERROR);
            return 0;
        }
        wchar_t format_status[160];
        _snwprintf_s(format_status,
                     sizeof(format_status) / sizeof(format_status[0]), _TRUNCATE,
                     L"Formatting as InfiltratorFS Format %u.%u ...",
                     (unsigned)INFS_FORMAT_MAJOR, (unsigned)INFS_FORMAT_MINOR);
        set_status(format_status);
        status = infs_format_storage(&storage, label);
        if (status != INFS_STATUS_OK) {
            infs_storage_close(&storage);
            set_status_code(L"Format volume", status);
            return 0;
        }
    }

    set_status(L"Opening InfiltratorFS volume ...");
    status = infs_volume_open_storage(&g_volume, &storage, 1);
    if (status != INFS_STATUS_OK) {
        infs_storage_close(&storage);
        set_status_code(L"Open InfiltratorFS volume", status);
        return 0;
    }
    g_volume_open = 1;
    update_buttons();
    refresh_contents();
    return 1;
}

static void add_files_dialog(void)
{
    if (!g_volume_open)
        return;
    const DWORD capacity = 65536u;
    wchar_t *buffer = calloc(capacity, sizeof(wchar_t));
    if (!buffer) {
        set_status_code(L"Allocate file dialog", INFS_STATUS_NO_MEMORY);
        return;
    }
    OPENFILENAMEW dialog;
    memset(&dialog, 0, sizeof(dialog));
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_main_window;
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = capacity;
    dialog.lpstrFilter = L"All files\0*.*\0\0";
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST |
                   OFN_ALLOWMULTISELECT | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&dialog)) {
        free(buffer);
        return;
    }

    wchar_t *first = buffer;
    wchar_t *next = first + wcslen(first) + 1u;
    int okay = 1;
    if (*next == 0) {
        okay = copy_host_path(first, "/");
    } else {
        wchar_t directory[32768];
        wcsncpy_s(directory, sizeof(directory) / sizeof(directory[0]),
                  first, _TRUNCATE);
        while (*next) {
            wchar_t full[32768];
            if (_snwprintf_s(full, sizeof(full) / sizeof(full[0]), _TRUNCATE,
                             L"%s\\%s", directory, next) < 0 ||
                !copy_host_path(full, "/")) {
                okay = 0;
                break;
            }
            next += wcslen(next) + 1u;
        }
    }
    free(buffer);
    if (okay)
        refresh_contents();
}

static void add_folder_dialog(void)
{
    if (!g_volume_open)
        return;
    BROWSEINFOW browse;
    memset(&browse, 0, sizeof(browse));
    browse.hwndOwner = g_main_window;
    browse.lpszTitle = L"Choose a folder to copy to the root of InfiltratorFS";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
    if (!item)
        return;
    wchar_t path[32768];
    if (SHGetPathFromIDListW(item, path) && copy_host_path(path, "/"))
        refresh_contents();
    CoTaskMemFree(item);
}

static void scrub_volume(void)
{
    if (!g_volume_open)
        return;
    struct infs_scrub_report report;
    set_status(L"Scrubbing InfiltratorFS volume ...");
    infs_status status = infs_scrub(&g_volume, &report);
    if (status != INFS_STATUS_OK) {
        set_status_code(L"Scrub volume", status);
        return;
    }
    wchar_t message[512];
    _snwprintf_s(message, sizeof(message) / sizeof(message[0]), _TRUNCATE,
                 L"Scrub complete.\n\nFiles checked: %llu\nData blocks checked: %llu\nChecksum errors: %llu\nMetadata errors: %llu",
                 (unsigned long long)report.files_checked,
                 (unsigned long long)report.data_blocks_checked,
                 (unsigned long long)report.checksum_errors,
                 (unsigned long long)report.metadata_errors);
    set_status(L"Scrub completed successfully.");
    MessageBoxW(g_main_window, message, L"InfiltratorFS Scrub",
                MB_OK | MB_ICONINFORMATION);
}

static void show_about(void)
{
    wchar_t message[1024];
    _snwprintf_s(message, sizeof(message) / sizeof(message[0]), _TRUNCATE,
                 L"InfiltratorFS Windows Transfer " INFILFS_VERSION_W
                 L"\n\nInfiltratorFS implementation " INFILFS_VERSION_W
                 L"\nDisk format: %u.%u\n\nPortable InfiltratorFS core with a native Windows storage/UI adapter.\nWindows discovery includes raw physical partitions that have no drive letter or Windows filesystem driver.\n\nLicence: GPL-3.0-or-later\n\nExperimental filesystem — use backed-up or disposable media while testing.",
                 (unsigned)INFS_FORMAT_MAJOR,
                 (unsigned)INFS_FORMAT_MINOR);
    MessageBoxW(g_main_window, message, L"About InfiltratorFS",
                MB_OK | MB_ICONINFORMATION);
}

static HMENU create_main_menu(void)
{
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU help = CreatePopupMenu();
    if (!menu || !file || !help)
        return menu;
    AppendMenuW(file, MF_STRING, IDM_FILE_REFRESH, L"&Refresh Volumes\tF5");
    AppendMenuW(file, MF_STRING, IDM_FILE_OPEN, L"&Open Selected Volume");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_FILE_EXIT, L"E&xit");
    AppendMenuW(help, MF_STRING, IDM_HELP_ABOUT, L"&About InfiltratorFS");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)help, L"&Help");
    return menu;
}

static void handle_drop(HDROP drop)
{
    if (!g_volume_open) {
        MessageBoxW(g_main_window,
                    L"Format or open an InfiltratorFS volume first.",
                    L"InfiltratorFS", MB_OK | MB_ICONINFORMATION);
        DragFinish(drop);
        return;
    }
    UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, NULL, 0);
    int okay = 1;
    for (UINT i = 0; i < count; ++i) {
        UINT length = DragQueryFileW(drop, i, NULL, 0);
        wchar_t *path = malloc(((size_t)length + 1u) * sizeof(wchar_t));
        if (!path) {
            okay = 0;
            break;
        }
        DragQueryFileW(drop, i, path, length + 1u);
        if (!copy_host_path(path, "/"))
            okay = 0;
        free(path);
        if (!okay)
            break;
    }
    DragFinish(drop);
    if (okay)
        refresh_contents();
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message,
                                    WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE: {
        g_main_window = hwnd;
        SetMenu(hwnd, create_main_menu());
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        CreateWindowW(L"STATIC", L"Target Windows volume / partition:",
                      WS_CHILD | WS_VISIBLE, 18, 18, 260, 22,
                      hwnd, NULL, NULL, NULL);
        HWND combo = CreateWindowW(L"COMBOBOX", L"",
                      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                      18, 42, 500, 360, hwnd, (HMENU)IDC_TARGET, NULL, NULL);
        HWND refresh = CreateWindowW(L"BUTTON", L"Refresh",
                      WS_CHILD | WS_VISIBLE, 530, 40, 90, 27,
                      hwnd, (HMENU)IDC_REFRESH, NULL, NULL);
        CreateWindowW(L"STATIC", L"Volume label:", WS_CHILD | WS_VISIBLE,
                      640, 18, 120, 22, hwnd, NULL, NULL, NULL);
        HWND label = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"InfiltratorFS",
                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                      640, 42, 255, 25,
                      hwnd, (HMENU)IDC_LABEL, NULL, NULL);
        HWND format = CreateWindowW(L"BUTTON", L"FORMAT selected volume",
                      WS_CHILD | WS_VISIBLE, 18, 82, 220, 34,
                      hwnd, (HMENU)IDC_FORMAT, NULL, NULL);
        HWND open = CreateWindowW(L"BUTTON", L"Open existing InfiltratorFS",
                      WS_CHILD | WS_VISIBLE, 250, 82, 225, 34,
                      hwnd, (HMENU)IDC_OPEN, NULL, NULL);
        HWND add_files = CreateWindowW(L"BUTTON", L"Add Files...",
                      WS_CHILD | WS_VISIBLE, 640, 82, 120, 34,
                      hwnd, (HMENU)IDC_ADD_FILES, NULL, NULL);
        HWND add_folder = CreateWindowW(L"BUTTON", L"Add Folder...",
                      WS_CHILD | WS_VISIBLE, 770, 82, 125, 34,
                      hwnd, (HMENU)IDC_ADD_FOLDER, NULL, NULL);
        HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                      18, 145, 877, 390, hwnd, (HMENU)IDC_CONTENTS, NULL, NULL);
        HWND scrub = CreateWindowW(L"BUTTON", L"Scrub / Verify",
                      WS_CHILD | WS_VISIBLE, 18, 548, 125, 30,
                      hwnd, (HMENU)IDC_SCRUB, NULL, NULL);
        HWND status = CreateWindowW(L"STATIC", L"",
                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                      155, 553, 740, 44, hwnd, (HMENU)IDC_STATUS, NULL, NULL);
        HWND controls[] = {combo, refresh, label, format, open,
                           add_files, add_folder, list, scrub, status};
        for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i)
            SendMessageW(controls[i], WM_SETFONT, (WPARAM)font, TRUE);
        DragAcceptFiles(hwnd, TRUE);
        refresh_volumes();
        update_buttons();
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_TARGET && HIWORD(wparam) == CBN_SELCHANGE) {
            update_buttons();
            return 0;
        }
        switch (LOWORD(wparam)) {
        case IDC_REFRESH:
        case IDM_FILE_REFRESH: refresh_volumes(); return 0;
        case IDC_FORMAT: open_selected_volume(1); return 0;
        case IDC_OPEN:
        case IDM_FILE_OPEN: open_selected_volume(0); return 0;
        case IDC_ADD_FILES: add_files_dialog(); return 0;
        case IDC_ADD_FOLDER: add_folder_dialog(); return 0;
        case IDC_SCRUB: scrub_volume(); return 0;
        case IDM_HELP_ABOUT: show_about(); return 0;
        case IDM_FILE_EXIT: DestroyWindow(hwnd); return 0;
        default: break;
        }
        break;
    case WM_DROPFILES:
        handle_drop((HDROP)wparam);
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_F5) {
            refresh_volumes();
            return 0;
        }
        break;
    case WM_DESTROY:
        close_volume();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous,
                    PWSTR command_line, int show_command)
{
    (void)previous;
    (void)command_line;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    const wchar_t class_name[] = L"InfiltratorFSWindowsTransfer";
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = class_name;
    if (!RegisterClassW(&wc))
        return 1;
    HWND window = CreateWindowW(
        class_name, L"InfiltratorFS Windows Transfer " INFILFS_VERSION_W,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 930, 640,
        NULL, NULL, instance, NULL);
    if (!window)
        return 1;
    ShowWindow(window, show_command);
    UpdateWindow(window);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return (int)msg.wParam;
}
#endif
