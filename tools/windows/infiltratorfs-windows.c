// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef _WIN32
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <winioctl.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include "infilfs/endian.h"
#include "infilfs/format_volume.h"
#include "infilfs/format.h"
#include "infilfs/check.h"
#include "infilfs/forensic.h"
#include "infilfs/fs.h"
#include "infilfs/status.h"
#include "infilfs/volume.h"
#include "infilfs/win32_io.h"
#include "infiltratorfs-windows-bridge.h"
#include "infiltratorfs-windows-metadata.h"
#include "../manager/infiltratorfs-manager-contract.h"
#include "infiltratr/arithmetic.h"
#include "infiltratr/design.h"
#include "infiltratr/format.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _MSC_VER
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#ifndef INFILFS_VERSION_W
#define INFILFS_VERSION_W L"0.9.6"
#endif

#define IDC_TARGET          1001
#define IDC_REFRESH         1002
#define IDC_LABEL           1003
#define IDC_FORMAT          1004
#define IDC_OPEN            1005
#define IDC_ADD_FILES       1006
#define IDC_ADD_FOLDER      1007
#define IDC_SCRUB           1008
#define IDC_CONTENTS        1009
#define IDC_STATUS          1010
#define IDC_MOUNT_DRIVE     1011
#define IDC_UNMOUNT_DRIVE   1012
#define IDC_TARGET_SUMMARY  1013
#define IDC_HEADER_TITLE    1014
#define IDC_HEADER_SUBTITLE 1015
#define IDC_STORAGE_HEADING 1016
#define IDC_CONTENTS_HEADING 1017
#define IDC_CONTENTS_HINT   1018
#define IDC_ACTIVITY_HEADING 1019
#define IDC_ACTIVITY        1020
#define IDC_ACTIVITY_CLEAR  1021
#define IDC_INSPECT         1022
#define IDC_OPEN_IMAGE      1023
#define IDC_LABEL_CAPTION   1024
#define IDC_NEW_IMAGE       1025
#define IDC_THEME           1026
#define IDC_ABOUT           1027
#define IDC_PAGE_OVERVIEW   1028
#define IDC_PAGE_FILES      1029
#define IDC_HERO_TITLE      1030
#define IDC_HERO_PATH       1031
#define IDC_MOUNT_BADGE     1032
#define IDC_STAT_CAPACITY_CAPTION 1033
#define IDC_STAT_CAPACITY   1034
#define IDC_STAT_FILESYSTEM_CAPTION 1035
#define IDC_STAT_FILESYSTEM 1036
#define IDC_STAT_STATUS_CAPTION 1037
#define IDC_STAT_STATUS     1038
#define IDC_VOLUME_INFO_HEADING 1039
#define IDC_DETAIL_TYPE_CAPTION 1040
#define IDC_DETAIL_TYPE     1041
#define IDC_DETAIL_PATH_CAPTION 1042
#define IDC_DETAIL_PATH     1043
#define IDC_DETAIL_FS_CAPTION 1044
#define IDC_DETAIL_FS       1045
#define IDC_DETAIL_LABEL_CAPTION 1046
#define IDC_DETAIL_LABEL    1047
#define IDC_DETAIL_MOUNT_CAPTION 1048
#define IDC_DETAIL_MOUNT    1049
#define IDC_DETAIL_MOUNTPOINT_CAPTION 1050
#define IDC_DETAIL_MOUNTPOINT 1051
#define IDC_MAINTENANCE_HEADING 1052
#define IDC_INSPECT_TITLE   1053
#define IDC_INSPECT_DESC    1054
#define IDC_CHECK           1055
#define IDC_CHECK_TITLE     1056
#define IDC_CHECK_DESC      1057
#define IDC_SCRUB_TITLE     1058
#define IDC_SCRUB_DESC      1059
#define IDC_FORENSIC        1060
#define IDC_FORENSIC_TITLE  1061
#define IDC_FORENSIC_DESC   1062
#define IDC_DANGER_HEADING  1063
#define IDC_DANGER_DESC     1064
#define IDC_STORAGE_COUNT   1065

#define IDM_FILE_REFRESH    2001
#define IDM_FILE_OPEN       2002
#define IDM_FILE_EXIT       2003
#define IDM_FILE_OPEN_IMAGE 2004
#define IDM_HELP_ABOUT      2101
#define IDM_VIEW_THEME_SYSTEM 2201
#define IDM_VIEW_THEME_DAY    2202
#define IDM_VIEW_THEME_NIGHT  2203

#define IDR_FONT_CORPO_A_COND_REGULAR 301
#define IDR_FONT_CORPO_S_BOLD          302
#define IDR_FONT_CORPO_S_REGULAR       303

#define MAX_TARGETS      256u
#define TARGET_PATH_MAX  4096u
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
    int is_image;
};

static struct infs_volume g_volume;
static int g_volume_open = 0;
static HWND g_main_window = NULL;
static struct target_volume g_targets[MAX_TARGETS];
static size_t g_target_count = 0;
static DWORD g_system_disks[MAX_SYSTEM_DISKS];
static size_t g_system_disk_count = 0;
static HFONT g_ui_font = NULL;
static HFONT g_title_font = NULL;
static HFONT g_heading_font = NULL;
static HFONT g_activity_font = NULL;
static HANDLE g_private_fonts[3] = { NULL, NULL, NULL };
static LONG g_copy_sequence = 0;
static int g_dark_mode = 0;
static InfiltratrThemeMode g_theme_mode = INFILTRATR_THEME_SYSTEM;
static COLORREF g_background_color;
static COLORREF g_panel_color;
static COLORREF g_card_color;
static COLORREF g_surface_color;
static COLORREF g_connection_color;
static COLORREF g_border_color;
static COLORREF g_text_color;
static COLORREF g_title_color;
static COLORREF g_heading_color;
static COLORREF g_summary_color;
static COLORREF g_kicker_color;
static COLORREF g_detail_color;
static COLORREF g_note_color;
static COLORREF g_muted_color;
static COLORREF g_accent_color;
static COLORREF g_success_color;
static COLORREF g_warning_color;
static COLORREF g_fault_color;
static COLORREF g_info_color;
static COLORREF g_warning_muted_color;
static COLORREF g_status_state_color;
static HBRUSH g_background_brush = NULL;
static HBRUSH g_panel_brush = NULL;
static HBRUSH g_card_brush = NULL;
static HBRUSH g_surface_brush = NULL;
static HBRUSH g_connection_brush = NULL;
static int g_show_files = 0;
static HIMAGELIST g_content_images = NULL;
static int g_icon_file = -1;
static int g_icon_folder = -1;
static int g_icon_link = -1;

static int open_selected_volume(int format_first);
static void update_target_summary(void);

static int system_prefers_dark_mode(void)
{
    DWORD value = 1;
    DWORD size = sizeof(value);
    LONG status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &value, &size);
    return status == ERROR_SUCCESS && value == 0;
}

static InfiltratrThemeMode load_theme_mode(void)
{
    DWORD value = (DWORD)INFILTRATR_THEME_SYSTEM;
    DWORD size = sizeof(value);
    LONG status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\InfiltratorProjects\\InfiltratorFS",
        L"ThemeMode", RRF_RT_REG_DWORD, NULL, &value, &size);
    if (status == ERROR_SUCCESS && value <= (DWORD)INFILTRATR_THEME_NIGHT)
        return (InfiltratrThemeMode)value;
    return INFILTRATR_THEME_SYSTEM;
}

static void save_theme_mode(InfiltratrThemeMode mode)
{
    DWORD value = (DWORD)mode;
    (void)RegSetKeyValueW(
        HKEY_CURRENT_USER,
        L"Software\\InfiltratorProjects\\InfiltratorFS",
        L"ThemeMode", REG_DWORD, &value, sizeof(value));
}

static COLORREF common_rgb(uint32_t rgb)
{
    return RGB((BYTE)((rgb >> 16) & 0xffu),
               (BYTE)((rgb >> 8) & 0xffu),
               (BYTE)(rgb & 0xffu));
}

static int utf8_to_wide_text(const char *text, wchar_t *out, size_t out_count)
{
    if (!text || !out || !out_count)
        return 0;
    return MultiByteToWideChar(
               CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
               out, (int)out_count) > 0;
}

static void set_control_text_utf8(HWND hwnd, int id, const char *text)
{
    wchar_t wide[512];
    if (utf8_to_wide_text(text, wide, sizeof(wide) / sizeof(wide[0])))
        SetWindowTextW(GetDlgItem(hwnd, id), wide);
}

static HWND create_static_utf8(HWND hwnd, int id, const char *text,
                               DWORD extra_style)
{
    wchar_t wide[1024];
    if (!utf8_to_wide_text(text, wide, sizeof(wide) / sizeof(wide[0])))
        wide[0] = L'\0';
    return CreateWindowW(
        L"STATIC", wide, WS_CHILD | WS_VISIBLE | SS_LEFT | extra_style,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)id, NULL, NULL);
}

static HWND create_button_utf8(HWND hwnd, int id, const char *text)
{
    wchar_t wide[256];
    if (!utf8_to_wide_text(text, wide, sizeof(wide) / sizeof(wide[0])))
        wide[0] = L'\0';
    return CreateWindowW(
        L"BUTTON", wide, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)id, NULL, NULL);
}

static void initialise_visual_theme(void)
{
    const int system_dark = system_prefers_dark_mode();
    const InfiltratrThemePalette *palette =
        infiltratr_theme_resolve(g_theme_mode, system_dark != 0);
    if (!palette)
        return;

    g_dark_mode =
        g_theme_mode == INFILTRATR_THEME_NIGHT ||
        (g_theme_mode == INFILTRATR_THEME_SYSTEM && system_dark);

    HBRUSH old_background = g_background_brush;
    HBRUSH old_panel = g_panel_brush;
    HBRUSH old_card = g_card_brush;
    HBRUSH old_surface = g_surface_brush;
    HBRUSH old_connection = g_connection_brush;

    g_background_color = common_rgb(palette->background_rgb);
    g_panel_color = common_rgb(palette->panel_rgb);
    g_card_color = common_rgb(palette->card_rgb);
    g_surface_color = common_rgb(palette->surface_rgb);
    g_connection_color = common_rgb(palette->connection_rgb);
    g_border_color = common_rgb(palette->border_rgb);
    g_text_color = common_rgb(palette->text_rgb);
    g_title_color = common_rgb(palette->title_rgb);
    g_heading_color = common_rgb(palette->heading_rgb);
    g_summary_color = common_rgb(palette->summary_rgb);
    g_kicker_color = common_rgb(palette->kicker_rgb);
    g_detail_color = common_rgb(palette->detail_label_rgb);
    g_note_color = common_rgb(palette->note_rgb);
    g_muted_color = common_rgb(palette->muted_rgb);
    g_accent_color = common_rgb(palette->neutral_accent_rgb);
    g_success_color = common_rgb(palette->success_rgb);
    g_warning_color = common_rgb(palette->warning_rgb);
    g_fault_color = common_rgb(palette->fault_rgb);
    g_info_color = common_rgb(palette->info_rgb);
    g_warning_muted_color = common_rgb(palette->warning_muted_rgb);
    g_status_state_color = g_warning_muted_color;

    g_background_brush = CreateSolidBrush(g_background_color);
    g_panel_brush = CreateSolidBrush(g_panel_color);
    g_card_brush = CreateSolidBrush(g_card_color);
    g_surface_brush = CreateSolidBrush(g_surface_color);
    g_connection_brush = CreateSolidBrush(g_connection_color);
    if (old_background) DeleteObject(old_background);
    if (old_panel) DeleteObject(old_panel);
    if (old_card) DeleteObject(old_card);
    if (old_surface) DeleteObject(old_surface);
    if (old_connection) DeleteObject(old_connection);
}

static void apply_window_visual_theme(HWND hwnd)
{
    BOOL dark = g_dark_mode ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    SetWindowTheme(hwnd, g_dark_mode ? L"DarkMode_Explorer" : L"Explorer",
                   NULL);
}

static void theme_control(HWND control)
{
    if (!control)
        return;
    SetWindowTheme(control,
                   g_dark_mode ? L"DarkMode_Explorer" : L"Explorer", NULL);
}

static void update_theme_menu(HWND hwnd)
{
    HMENU menu = GetMenu(hwnd);
    HMENU view = menu ? GetSubMenu(menu, 1) : NULL;
    HMENU theme = view ? GetSubMenu(view, 0) : NULL;
    if (!theme)
        return;
    UINT selected = IDM_VIEW_THEME_SYSTEM;
    if (g_theme_mode == INFILTRATR_THEME_DAY)
        selected = IDM_VIEW_THEME_DAY;
    else if (g_theme_mode == INFILTRATR_THEME_NIGHT)
        selected = IDM_VIEW_THEME_NIGHT;
    CheckMenuRadioItem(theme, IDM_VIEW_THEME_SYSTEM, IDM_VIEW_THEME_NIGHT,
                       selected, MF_BYCOMMAND);
}

static void apply_current_theme(HWND hwnd)
{
    initialise_visual_theme();
    apply_window_visual_theme(hwnd);

    HWND list = GetDlgItem(hwnd, IDC_CONTENTS);
    if (list) {
        ListView_SetBkColor(list, g_connection_color);
        ListView_SetTextBkColor(list, g_connection_color);
        ListView_SetTextColor(list, g_text_color);
        theme_control(list);
        theme_control(ListView_GetHeader(list));
    }

    int themed_ids[] = {
        IDC_TARGET, IDC_REFRESH, IDC_LABEL, IDC_FORMAT, IDC_OPEN_IMAGE,
        IDC_OPEN, IDC_INSPECT, IDC_ADD_FILES, IDC_ADD_FOLDER, IDC_SCRUB,
        IDC_MOUNT_DRIVE, IDC_UNMOUNT_DRIVE, IDC_ACTIVITY_CLEAR, IDC_ACTIVITY
    };
    for (size_t i = 0;
         i < sizeof(themed_ids) / sizeof(themed_ids[0]); ++i)
        theme_control(GetDlgItem(hwnd, themed_ids[i]));

    update_theme_menu(hwnd);
    update_target_summary();
    SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND, (LONG_PTR)g_background_brush);
    RedrawWindow(hwnd, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static void set_theme_mode(HWND hwnd, InfiltratrThemeMode mode)
{
    g_theme_mode = mode;
    save_theme_mode(mode);
    apply_current_theme(hwnd);
}

static int add_stock_icon(HIMAGELIST list, SHSTOCKICONID stock)
{
    SHSTOCKICONINFO info;
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    if (FAILED(SHGetStockIconInfo(stock, SHGSI_ICON | SHGSI_SMALLICON,
                                  &info)))
        return -1;
    int index = ImageList_AddIcon(list, info.hIcon);
    DestroyIcon(info.hIcon);
    return index;
}

static void append_activity(const wchar_t *text)
{
    if (!g_main_window || !text || !text[0])
        return;
    HWND edit = GetDlgItem(g_main_window, IDC_ACTIVITY);
    if (!edit)
        return;
    int length = GetWindowTextLengthW(edit);
    SendMessageW(edit, EM_SETSEL, (WPARAM)length, (LPARAM)length);
    SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
}

static void set_status(const wchar_t *text)
{
    if (!g_main_window)
        return;
    HWND control = GetDlgItem(g_main_window, IDC_STATUS);
    SetWindowTextW(control, text ? text : L"");
    UpdateWindow(control);
    if (text && text[0])
        append_activity(text);
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

static void set_windows_error(const wchar_t *action, DWORD error)
{
    wchar_t detail[512] = L"Windows error";
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, error, 0, detail,
        (DWORD)(sizeof(detail) / sizeof(detail[0])), NULL);
    while (length && (detail[length - 1u] == L'\r' ||
                      detail[length - 1u] == L'\n' ||
                      detail[length - 1u] == L' ' ||
                      detail[length - 1u] == L'.'))
        detail[--length] = L'\0';

    wchar_t message[896];
    _snwprintf_s(message, sizeof(message) / sizeof(message[0]), _TRUNCATE,
                 L"%s failed: %s (Windows error %lu)",
                 action, detail, (unsigned long)error);
    set_status(message);
    MessageBoxW(g_main_window, message, L"InfiltratorFS",
                MB_OK | MB_ICONERROR);
}

static void close_volume(void)
{
    if (infs_windows_bridge_active())
        infs_windows_bridge_stop();
    if (g_volume_open) {
        infs_volume_close(&g_volume);
        memset(&g_volume, 0, sizeof(g_volume));
        g_volume_open = 0;
    }
}

static struct target_volume *selected_target(void)
{
    HWND list = GetDlgItem(g_main_window, IDC_TARGET);
    LRESULT selected = SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (selected == LB_ERR)
        return NULL;
    LRESULT data = SendMessageW(list, LB_GETITEMDATA, (WPARAM)selected, 0);
    if (data == LB_ERR || data < 0 || (size_t)data >= g_target_count)
        return NULL;
    return &g_targets[(size_t)data];
}



static int register_embedded_font(HINSTANCE instance, WORD resource_id,
                                  size_t slot)
{
    HRSRC resource = FindResourceW(
        instance, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (!resource)
        return 0;
    HGLOBAL loaded = LoadResource(instance, resource);
    if (!loaded)
        return 0;
    DWORD size = SizeofResource(instance, resource);
    void *data = LockResource(loaded);
    if (!data || !size)
        return 0;
    DWORD count = 0;
    HANDLE font = AddFontMemResourceEx(data, size, NULL, &count);
    if (!font || count == 0)
        return 0;
    g_private_fonts[slot] = font;
    return 1;
}

static int register_project_fonts(HINSTANCE instance)
{
    static const WORD resources[] = {
        IDR_FONT_CORPO_A_COND_REGULAR,
        IDR_FONT_CORPO_S_BOLD,
        IDR_FONT_CORPO_S_REGULAR
    };
    for (size_t index = 0;
         index < sizeof(resources) / sizeof(resources[0]); ++index) {
        if (!register_embedded_font(instance, resources[index], index))
            return 0;
    }
    return 1;
}

static void unregister_project_fonts(void)
{
    for (size_t index = 0;
         index < sizeof(g_private_fonts) / sizeof(g_private_fonts[0]); ++index) {
        if (g_private_fonts[index]) {
            RemoveFontMemResourceEx(g_private_fonts[index]);
            g_private_fonts[index] = NULL;
        }
    }
}

static HFONT create_ui_font(HWND hwnd, int points, int weight,
                            const wchar_t *family)
{
    HDC dc = GetDC(hwnd);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc)
        ReleaseDC(hwnd, dc);
    return CreateFontW(-MulDiv(points, dpi, 72), 0, 0, 0, weight,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       family);
}

static HFONT create_common_font(HWND hwnd, int points, const char *family,
                                uint32_t weight)
{
    wchar_t wide_family[LF_FACESIZE];
    if (!family ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, family, -1,
                             wide_family,
                             (int)(sizeof(wide_family) /
                                   sizeof(wide_family[0]))))
        return NULL;
    return create_ui_font(hwnd, points, (int)weight, wide_family);
}

static void set_control_font(HWND hwnd, int id, HFONT font)
{
    HWND control = GetDlgItem(hwnd, id);
    if (control && font)
        SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
}

static int format_capacity_wide(uint64_t bytes, wchar_t *out,
                                size_t out_count)
{
    char text[64];
    if (!out || !out_count ||
        !infilfs_manager_format_capacity(bytes, text, sizeof(text)))
        return 0;
    return MultiByteToWideChar(
               CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
               out, (int)out_count) > 0;
}

static void update_target_summary(void)
{
    if (!g_main_window)
        return;

    const struct infilfs_manager_copy *copy = infilfs_manager_copy();
    struct target_volume *target = selected_target();
    if (!target) {
        set_control_text_utf8(
            g_main_window, IDC_HERO_TITLE, copy->empty_title);
        SetWindowTextW(GetDlgItem(g_main_window, IDC_HERO_PATH), L"");
        set_control_text_utf8(
            g_main_window, IDC_MOUNT_BADGE, copy->unmounted_status);
        SetWindowTextW(GetDlgItem(g_main_window, IDC_STAT_CAPACITY), L"—");
        SetWindowTextW(GetDlgItem(g_main_window, IDC_STAT_FILESYSTEM), L"—");
        SetWindowTextW(GetDlgItem(g_main_window, IDC_STAT_STATUS), L"—");
        int detail_ids[] = {
            IDC_DETAIL_TYPE, IDC_DETAIL_PATH, IDC_DETAIL_FS,
            IDC_DETAIL_LABEL, IDC_DETAIL_MOUNT, IDC_DETAIL_MOUNTPOINT
        };
        for (size_t i = 0; i < sizeof(detail_ids) / sizeof(detail_ids[0]); ++i)
            SetWindowTextW(GetDlgItem(g_main_window, detail_ids[i]), L"—");
        g_status_state_color = g_warning_muted_color;
        return;
    }

    wchar_t size_text[64];
    if (!format_capacity_wide(
            target->size_bytes, size_text,
            sizeof(size_text) / sizeof(size_text[0])))
        wcscpy_s(size_text, sizeof(size_text) / sizeof(size_text[0]),
                 L"0 B");

    const wchar_t *label = target->infs_label[0] ?
                           target->infs_label : L"InfiltratorFS";
    wchar_t title[INFS_LABEL_MAX + 96u];
    if (target->is_infiltrator)
        wcsncpy_s(title, sizeof(title) / sizeof(title[0]), label, _TRUNCATE);
    else if (target->is_image) {
        const wchar_t *base = wcsrchr(target->device_path, L'\\');
        wcsncpy_s(title, sizeof(title) / sizeof(title[0]),
                  base ? base + 1 : target->device_path, _TRUNCATE);
    } else if (target->use_region) {
        _snwprintf_s(title, sizeof(title) / sizeof(title[0]), _TRUNCATE,
                     L"Disk %lu · Partition %lu",
                     (unsigned long)target->disk_number,
                     (unsigned long)target->partition_number);
    } else {
        wcsncpy_s(title, sizeof(title) / sizeof(title[0]),
                  target->mount_point[0] ? target->mount_point :
                  target->device_path, _TRUNCATE);
    }
    SetWindowTextW(GetDlgItem(g_main_window, IDC_HERO_TITLE), title);
    SetWindowTextW(GetDlgItem(g_main_window, IDC_HERO_PATH),
                   target->device_path);

    const int mounted = infs_windows_bridge_active();
    set_control_text_utf8(
        g_main_window, IDC_MOUNT_BADGE,
        mounted ? copy->mounted_status : copy->unmounted_status);
    SetWindowTextW(GetDlgItem(g_main_window, IDC_STAT_CAPACITY), size_text);
    set_control_text_utf8(
        g_main_window, IDC_STAT_STATUS,
        mounted ? copy->mounted_status : copy->unmounted_status);
    g_status_state_color = mounted ? g_success_color : g_warning_muted_color;

    wchar_t fs_text[96];
    if (target->is_infiltrator) {
        _snwprintf_s(fs_text, sizeof(fs_text) / sizeof(fs_text[0]), _TRUNCATE,
                     L"InfiltratorFS %u.%u",
                     (unsigned)target->format_major,
                     (unsigned)target->format_minor);
    } else {
        (void)utf8_to_wide_text(
            copy->unknown_filesystem, fs_text,
            sizeof(fs_text) / sizeof(fs_text[0]));
    }
    SetWindowTextW(GetDlgItem(g_main_window, IDC_STAT_FILESYSTEM), fs_text);

    if (target->is_image)
        set_control_text_utf8(
            g_main_window, IDC_DETAIL_TYPE, copy->image_file_type);
    else
        SetWindowTextW(
            GetDlgItem(g_main_window, IDC_DETAIL_TYPE),
            target->use_region ? L"Physical partition" : L"Windows volume");
    SetWindowTextW(GetDlgItem(g_main_window, IDC_DETAIL_PATH),
                   target->device_path);
    SetWindowTextW(GetDlgItem(g_main_window, IDC_DETAIL_FS), fs_text);
    SetWindowTextW(GetDlgItem(g_main_window, IDC_DETAIL_LABEL),
                   target->is_infiltrator ? label : L"—");
    set_control_text_utf8(
        g_main_window, IDC_DETAIL_MOUNT,
        mounted ? copy->mounted_status : copy->unmounted_status);

    wchar_t bridge_root[MAX_PATH * 4u] = L"";
    if (mounted)
        (void)infs_windows_bridge_root(
            bridge_root, sizeof(bridge_root) / sizeof(bridge_root[0]));
    SetWindowTextW(GetDlgItem(g_main_window, IDC_DETAIL_MOUNTPOINT),
                   mounted && bridge_root[0] ? bridge_root :
                   (target->mount_point[0] ? target->mount_point : L"—"));

    if (target->is_infiltrator)
        SetWindowTextW(GetDlgItem(g_main_window, IDC_LABEL), label);

    wchar_t summary[512];
    _snwprintf_s(summary, sizeof(summary) / sizeof(summary[0]), _TRUNCATE,
                 L"%s  ·  %s  ·  %s",
                 title, size_text, fs_text);
    SetWindowTextW(GetDlgItem(g_main_window, IDC_TARGET_SUMMARY), summary);

    (void)copy;
    InvalidateRect(g_main_window, NULL, TRUE);
}

static void show_page(int files)
{
    g_show_files = files ? 1 : 0;
    const int overview_ids[] = {
        IDC_STAT_CAPACITY_CAPTION, IDC_STAT_CAPACITY,
        IDC_STAT_FILESYSTEM_CAPTION, IDC_STAT_FILESYSTEM,
        IDC_STAT_STATUS_CAPTION, IDC_STAT_STATUS,
        IDC_VOLUME_INFO_HEADING,
        IDC_DETAIL_TYPE_CAPTION, IDC_DETAIL_TYPE,
        IDC_DETAIL_PATH_CAPTION, IDC_DETAIL_PATH,
        IDC_DETAIL_FS_CAPTION, IDC_DETAIL_FS,
        IDC_DETAIL_LABEL_CAPTION, IDC_DETAIL_LABEL,
        IDC_DETAIL_MOUNT_CAPTION, IDC_DETAIL_MOUNT,
        IDC_DETAIL_MOUNTPOINT_CAPTION, IDC_DETAIL_MOUNTPOINT,
        IDC_MAINTENANCE_HEADING,
        IDC_INSPECT_TITLE, IDC_INSPECT_DESC, IDC_INSPECT,
        IDC_CHECK_TITLE, IDC_CHECK_DESC, IDC_CHECK,
        IDC_SCRUB_TITLE, IDC_SCRUB_DESC, IDC_SCRUB,
        IDC_FORENSIC_TITLE, IDC_FORENSIC_DESC, IDC_FORENSIC,
        IDC_DANGER_HEADING, IDC_DANGER_DESC,
        IDC_LABEL_CAPTION, IDC_LABEL, IDC_FORMAT
    };
    const int files_ids[] = {
        IDC_CONTENTS_HEADING, IDC_CONTENTS_HINT,
        IDC_ADD_FILES, IDC_ADD_FOLDER, IDC_CONTENTS
    };
    for (size_t i = 0; i < sizeof(overview_ids) / sizeof(overview_ids[0]); ++i)
        ShowWindow(GetDlgItem(g_main_window, overview_ids[i]),
                   files ? SW_HIDE : SW_SHOW);
    for (size_t i = 0; i < sizeof(files_ids) / sizeof(files_ids[0]); ++i)
        ShowWindow(GetDlgItem(g_main_window, files_ids[i]),
                   files ? SW_SHOW : SW_HIDE);
    EnableWindow(GetDlgItem(g_main_window, IDC_PAGE_OVERVIEW), files);
    EnableWindow(GetDlgItem(g_main_window, IDC_PAGE_FILES), !files);
}

static void layout_controls(HWND hwnd)
{
    RECT rect;
    GetClientRect(hwnd, &rect);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    const int margin = 20;
    const int sidebar = 286;
    const int gap = 24;
    const int header_top = 14;
    const int body_top = 82;
    const int right = margin + sidebar + gap;
    int right_width = width - right - margin;
    int status_y = height - 38;
    const int activity_height = 116;
    int activity_y = status_y - activity_height - 12;
    int page_top = body_top + 112;
    int page_bottom = activity_y - 12;
    int page_height = page_bottom - page_top;
    if (page_height < 320)
        page_height = 320;

    MoveWindow(GetDlgItem(hwnd, IDC_HEADER_TITLE),
               margin, header_top, 280, 30, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_HEADER_SUBTITLE),
               margin + 2, header_top + 31, 340, 20, TRUE);

    int header_x = width - margin;
    header_x -= 86;
    MoveWindow(GetDlgItem(hwnd, IDC_ABOUT), header_x, header_top + 8, 86, 32, TRUE);
    header_x -= 94;
    MoveWindow(GetDlgItem(hwnd, IDC_THEME), header_x, header_top + 8, 86, 32, TRUE);
    header_x -= 94;
    MoveWindow(GetDlgItem(hwnd, IDC_REFRESH), header_x, header_top + 8, 86, 32, TRUE);
    header_x -= 108;
    MoveWindow(GetDlgItem(hwnd, IDC_OPEN_IMAGE), header_x, header_top + 8, 100, 32, TRUE);
    header_x -= 104;
    MoveWindow(GetDlgItem(hwnd, IDC_NEW_IMAGE), header_x, header_top + 8, 96, 32, TRUE);

    MoveWindow(GetDlgItem(hwnd, IDC_STORAGE_HEADING),
               margin, body_top, sidebar, 22, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_TARGET),
               margin, body_top + 30, sidebar, activity_y - body_top - 62, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_STORAGE_COUNT),
               margin, activity_y - 24, sidebar, 20, TRUE);

    MoveWindow(GetDlgItem(hwnd, IDC_HERO_TITLE),
               right, body_top, right_width - 320, 32, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_MOUNT_BADGE),
               right + right_width - 312, body_top + 2, 92, 26, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_HERO_PATH),
               right, body_top + 36, right_width - 260, 20, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_MOUNT_DRIVE),
               right + right_width - 210, body_top + 30, 130, 32, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_UNMOUNT_DRIVE),
               right + right_width - 74, body_top + 30, 74, 32, TRUE);

    MoveWindow(GetDlgItem(hwnd, IDC_PAGE_OVERVIEW),
               right, body_top + 70, 98, 30, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_PAGE_FILES),
               right + 104, body_top + 70, 72, 30, TRUE);

    int stat_y = page_top;
    int stat_gap = 10;
    int stat_w = (right_width - 2 * stat_gap) / 3;
    int stat_ids[][2] = {
        { IDC_STAT_CAPACITY_CAPTION, IDC_STAT_CAPACITY },
        { IDC_STAT_FILESYSTEM_CAPTION, IDC_STAT_FILESYSTEM },
        { IDC_STAT_STATUS_CAPTION, IDC_STAT_STATUS }
    };
    for (int i = 0; i < 3; ++i) {
        int sx = right + i * (stat_w + stat_gap);
        MoveWindow(GetDlgItem(hwnd, stat_ids[i][0]), sx + 12, stat_y + 8, stat_w - 24, 16, TRUE);
        MoveWindow(GetDlgItem(hwnd, stat_ids[i][1]), sx + 12, stat_y + 28, stat_w - 24, 25, TRUE);
    }

    int info_y = stat_y + 68;
    int left_w = (right_width * 55) / 100;
    int right_x = right + left_w + 18;
    int maint_w = right_width - left_w - 18;
    MoveWindow(GetDlgItem(hwnd, IDC_VOLUME_INFO_HEADING), right, info_y, left_w, 24, TRUE);
    int row_y = info_y + 32;
    const int cap_ids[] = {
        IDC_DETAIL_TYPE_CAPTION, IDC_DETAIL_PATH_CAPTION, IDC_DETAIL_FS_CAPTION,
        IDC_DETAIL_LABEL_CAPTION, IDC_DETAIL_MOUNT_CAPTION, IDC_DETAIL_MOUNTPOINT_CAPTION
    };
    const int value_ids[] = {
        IDC_DETAIL_TYPE, IDC_DETAIL_PATH, IDC_DETAIL_FS,
        IDC_DETAIL_LABEL, IDC_DETAIL_MOUNT, IDC_DETAIL_MOUNTPOINT
    };
    for (int i = 0; i < 6; ++i) {
        MoveWindow(GetDlgItem(hwnd, cap_ids[i]), right, row_y, 118, 20, TRUE);
        MoveWindow(GetDlgItem(hwnd, value_ids[i]), right + 124, row_y,
                   left_w - 124, 20, TRUE);
        row_y += 28;
    }

    MoveWindow(GetDlgItem(hwnd, IDC_MAINTENANCE_HEADING), right_x, info_y, maint_w, 24, TRUE);
    const int action_title_ids[] = {
        IDC_INSPECT_TITLE, IDC_CHECK_TITLE, IDC_SCRUB_TITLE, IDC_FORENSIC_TITLE
    };
    const int action_desc_ids[] = {
        IDC_INSPECT_DESC, IDC_CHECK_DESC, IDC_SCRUB_DESC, IDC_FORENSIC_DESC
    };
    const int action_button_ids[] = {
        IDC_INSPECT, IDC_CHECK, IDC_SCRUB, IDC_FORENSIC
    };
    row_y = info_y + 32;
    for (int i = 0; i < 4; ++i) {
        MoveWindow(GetDlgItem(hwnd, action_title_ids[i]), right_x, row_y, maint_w - 82, 20, TRUE);
        MoveWindow(GetDlgItem(hwnd, action_desc_ids[i]), right_x, row_y + 19, maint_w - 82, 34, TRUE);
        MoveWindow(GetDlgItem(hwnd, action_button_ids[i]), right_x + maint_w - 76, row_y + 7, 76, 32, TRUE);
        row_y += 58;
    }

    int danger_y = page_bottom - 82;
    if (danger_y < info_y + 210)
        danger_y = info_y + 210;
    MoveWindow(GetDlgItem(hwnd, IDC_DANGER_HEADING), right, danger_y, 170, 24, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_DANGER_DESC), right, danger_y + 26, right_width - 420, 40, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_LABEL_CAPTION), right + right_width - 410, danger_y, 80, 20, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_LABEL), right + right_width - 330, danger_y - 3, 190, 28, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_FORMAT), right + right_width - 132, danger_y - 3, 132, 30, TRUE);

    MoveWindow(GetDlgItem(hwnd, IDC_CONTENTS_HEADING),
               right, page_top, right_width, 24, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_CONTENTS_HINT),
               right, page_top + 26, right_width - 250, 20, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_ADD_FILES),
               right + right_width - 224, page_top + 2, 104, 32, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_ADD_FOLDER),
               right + right_width - 112, page_top + 2, 112, 32, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_CONTENTS),
               right, page_top + 56, right_width, page_height - 56, TRUE);

    HWND list = GetDlgItem(hwnd, IDC_CONTENTS);
    if (list) {
        int type_width = 120;
        int name_width = right_width - type_width - 8;
        if (name_width < 220) name_width = 220;
        ListView_SetColumnWidth(list, 0, name_width);
        ListView_SetColumnWidth(list, 1, type_width);
    }

    MoveWindow(GetDlgItem(hwnd, IDC_ACTIVITY_HEADING),
               right, activity_y, right_width - 82, 22, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_ACTIVITY_CLEAR),
               right + right_width - 72, activity_y - 4, 72, 28, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_ACTIVITY),
               right, activity_y + 28, right_width, activity_height - 28, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_STATUS),
               margin, status_y, width - 2 * margin, 28, TRUE);

    show_page(g_show_files);
}

static void update_buttons(void)
{
    struct target_volume *target = selected_target();
    const int bridge_active = infs_windows_bridge_active();
    const struct infilfs_manager_state state = {
        .has_target = target != NULL,
        .is_infiltrator = target && target->is_infiltrator,
        .mounted = bridge_active != 0,
        .busy = false,
        .volume_open = g_volume_open != 0
    };
    struct infilfs_manager_enablement enabled;
    infilfs_manager_compute_enablement(&state, &enabled);

    update_target_summary();
    EnableWindow(GetDlgItem(g_main_window, IDC_FORMAT), enabled.format);
    EnableWindow(GetDlgItem(g_main_window, IDC_ADD_FILES), enabled.file_mutation);
    EnableWindow(GetDlgItem(g_main_window, IDC_ADD_FOLDER), enabled.file_mutation);
    EnableWindow(GetDlgItem(g_main_window, IDC_INSPECT), enabled.inspect);
    EnableWindow(GetDlgItem(g_main_window, IDC_CHECK), enabled.maintenance);
    EnableWindow(GetDlgItem(g_main_window, IDC_SCRUB), enabled.maintenance);
    EnableWindow(GetDlgItem(g_main_window, IDC_FORENSIC), enabled.maintenance);
    EnableWindow(GetDlgItem(g_main_window, IDC_MOUNT_DRIVE), enabled.mount);
    EnableWindow(GetDlgItem(g_main_window, IDC_UNMOUNT_DRIVE), enabled.unmount);
    EnableWindow(GetDlgItem(g_main_window, IDC_PAGE_FILES), enabled.files);

    if (bridge_active)
        SetWindowTextW(
            GetDlgItem(g_main_window, IDC_MOUNT_DRIVE), L"Open in Explorer");
    else
        set_control_text_utf8(
            g_main_window, IDC_MOUNT_DRIVE,
            infilfs_manager_copy()->mount_button);
    set_control_text_utf8(g_main_window, IDC_UNMOUNT_DRIVE,
                          infilfs_manager_copy()->unmount_button);
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
        size_t next_capacity = 0;
        if (!infiltratr_size_multiply_checked(
                capacity, 2u, &next_capacity) ||
            next_capacity > UINT32_MAX)
            break;
        capacity = next_capacity;
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

static int add_combo_target(HWND list, struct target_volume *candidate,
                            const wchar_t *display)
{
    if (g_target_count >= MAX_TARGETS)
        return 0;
    size_t target_index = g_target_count;
    g_targets[target_index] = *candidate;
    LRESULT item_index = SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)display);
    if (item_index == LB_ERR || item_index == LB_ERRSPACE)
        return 0;
    SendMessageW(list, LB_SETITEMDATA, (WPARAM)item_index,
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
    wchar_t size_text[64];
    if (!format_capacity_wide(
            candidate.size_bytes, size_text,
            sizeof(size_text) / sizeof(size_text[0])))
        wcscpy_s(size_text, sizeof(size_text) / sizeof(size_text[0]),
                 L"0 B");
    if (candidate.is_infiltrator) {
        _snwprintf_s(display, sizeof(display) / sizeof(display[0]), _TRUNCATE,
                     L"[InfiltratorFS %u.%u]  %s  %s  %s",
                     (unsigned)candidate.format_major,
                     (unsigned)candidate.format_minor,
                     location, size_text,
                     candidate.infs_label[0] ? candidate.infs_label : L"InfiltratorFS");
    } else {
        _snwprintf_s(display, sizeof(display) / sizeof(display[0]), _TRUNCATE,
                     L"%s  %s  %s%s%s",
                     location, windows_fs, size_text,
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
        size_t next_capacity = 0;
        if (!infiltratr_size_multiply_checked(
                capacity, 2u, &next_capacity) ||
            next_capacity > UINT32_MAX)
            return NULL;
        capacity = next_capacity;
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
    wchar_t size_text[64];
    if (!format_capacity_wide(
            size_bytes, size_text,
            sizeof(size_text) / sizeof(size_text[0])))
        wcscpy_s(size_text, sizeof(size_text) / sizeof(size_text[0]),
                 L"0 B");
    if (candidate.is_infiltrator) {
        _snwprintf_s(display, sizeof(display) / sizeof(display[0]), _TRUNCATE,
                     L"[InfiltratorFS %u.%u]  Disk %lu partition %lu  %s  %s",
                     (unsigned)candidate.format_major,
                     (unsigned)candidate.format_minor,
                     (unsigned long)disk_number,
                     (unsigned long)part->PartitionNumber,
                     size_text,
                     candidate.infs_label[0] ? candidate.infs_label : L"InfiltratorFS");
    } else {
        _snwprintf_s(display, sizeof(display) / sizeof(display[0]), _TRUNCATE,
                     L"[RAW removable partition]  Disk %lu partition %lu  %s",
                     (unsigned long)disk_number,
                     (unsigned long)part->PartitionNumber,
                     size_text);
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

static int add_image_target_from_path(const wchar_t *path)
{
    HANDLE file = CreateFileW(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        set_windows_error(L"Open image file", GetLastError());
        return 0;
    }
    LARGE_INTEGER length;
    if (!GetFileSizeEx(file, &length) || length.QuadPart <= 0) {
        DWORD error = GetLastError();
        CloseHandle(file);
        if (!error) error = ERROR_BAD_LENGTH;
        set_windows_error(L"Read image size", error);
        return 0;
    }
    CloseHandle(file);
    if (wcslen(path) >= TARGET_PATH_MAX) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        set_windows_error(L"Open image path", GetLastError());
        return 0;
    }

    struct target_volume candidate;
    memset(&candidate, 0, sizeof(candidate));
    wcsncpy_s(candidate.device_path, TARGET_PATH_MAX, path, _TRUNCATE);
    candidate.size_bytes = (uint64_t)length.QuadPart;
    candidate.is_image = 1;

    struct infs_superblock_disk sb;
    memset(&sb, 0, sizeof(sb));
    uint64_t probed_size = 0;
    candidate.is_infiltrator =
        probe_infiltratorfs_volume(path, &probed_size, &sb);
    if (candidate.is_infiltrator) {
        candidate.format_major = infs_le16_to_cpu(sb.format_major);
        candidate.format_minor = infs_le16_to_cpu(sb.format_minor);
        superblock_label_to_wide(&sb, candidate.infs_label);
    }

    const wchar_t *base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    wchar_t display[512];
    wchar_t size_text[64];
    if (!format_capacity_wide(candidate.size_bytes, size_text,
                         sizeof(size_text) / sizeof(size_text[0])))
        wcscpy_s(size_text, sizeof(size_text) / sizeof(size_text[0]),
                 L"0 B");
    if (candidate.is_infiltrator) {
        _snwprintf_s(display, sizeof(display) / sizeof(display[0]), _TRUNCATE,
                     L"[Image · InfiltratorFS %u.%u]  %s  %s",
                     (unsigned)candidate.format_major,
                     (unsigned)candidate.format_minor, base, size_text);
    } else {
        _snwprintf_s(display, sizeof(display) / sizeof(display[0]), _TRUNCATE,
                     L"[Image]  %s  %s", base, size_text);
    }

    HWND list = GetDlgItem(g_main_window, IDC_TARGET);
    if (!add_combo_target(list, &candidate, display))
        return 0;
    SendMessageW(list, LB_SETCURSEL,
                 (WPARAM)(SendMessageW(list, LB_GETCOUNT, 0, 0) - 1), 0);
    update_buttons();
    return 1;
}

static void open_image_dialog(void)
{
    wchar_t path[32768] = L"";
    OPENFILENAMEW dialog;
    memset(&dialog, 0, sizeof(dialog));
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_main_window;
    dialog.lpstrFile = path;
    dialog.nMaxFile = (DWORD)(sizeof(path) / sizeof(path[0]));
    dialog.lpstrFilter =
        L"InfiltratorFS images\0*.img;*.infiltratorfs\0"
        L"All files\0*.*\0\0";
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&dialog))
        return;
    close_volume();
    if (add_image_target_from_path(path))
        set_status(L"Image added to the storage list.");
}

static void create_image_dialog(void)
{
    wchar_t path[32768] = L"";
    (void)utf8_to_wide_text(
        infilfs_manager_copy()->default_image_name,
        path, sizeof(path) / sizeof(path[0]));
    OPENFILENAMEW dialog;
    memset(&dialog, 0, sizeof(dialog));
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_main_window;
    dialog.lpstrFile = path;
    dialog.nMaxFile = (DWORD)(sizeof(path) / sizeof(path[0]));
    dialog.lpstrFilter =
        L"InfiltratorFS image\0*.img\0All files\0*.*\0\0";
    dialog.lpstrDefExt = L"img";
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&dialog))
        return;

    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        set_windows_error(L"Create image file", GetLastError());
        return;
    }
    LARGE_INTEGER size;
    size.QuadPart = INT64_C(512) * 1024 * 1024;
    if (!SetFilePointerEx(file, size, NULL, FILE_BEGIN) || !SetEndOfFile(file)) {
        DWORD error = GetLastError();
        CloseHandle(file);
        DeleteFileW(path);
        set_windows_error(L"Size image file", error);
        return;
    }
    CloseHandle(file);

    wchar_t wide_label[INFS_LABEL_MAX] = L"InfiltratorFS";
    GetWindowTextW(GetDlgItem(g_main_window, IDC_LABEL), wide_label,
                   (int)(sizeof(wide_label) / sizeof(wide_label[0])));
    char label[INFS_LABEL_MAX * 4u];
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_label, -1,
                             label, (int)sizeof(label), NULL, NULL)) {
        DeleteFileW(path);
        MessageBoxW(g_main_window, L"The volume label is not valid Unicode.",
                    L"InfiltratorFS", MB_OK | MB_ICONERROR);
        return;
    }

    struct infs_storage storage = {0};
    infs_status status = infs_win32_storage_open(&storage, path, 1, 0);
    if (status == INFS_STATUS_OK)
        status = infs_format_storage(&storage, label);
    infs_storage_close(&storage);
    if (status != INFS_STATUS_OK) {
        DeleteFileW(path);
        set_status_code(L"Create image", status);
        return;
    }
    close_volume();
    if (add_image_target_from_path(path)) {
        set_status(L"512 MiB image created and formatted successfully.");
        (void)open_selected_volume(0);
        update_buttons();
    }
}

static void refresh_volumes(void)
{
    if (infs_windows_bridge_active())
        infs_windows_bridge_stop();
    close_volume();
    HWND combo = GetDlgItem(g_main_window, IDC_TARGET);
    SendMessageW(combo, LB_RESETCONTENT, 0, 0);
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
        SendMessageW(combo, LB_SETCURSEL, 0, 0);
    wchar_t count_text[96];
    _snwprintf_s(count_text, sizeof(count_text) / sizeof(count_text[0]),
                 _TRUNCATE, L"%d available partition%s",
                 added, added == 1 ? L"" : L"s");
    SetWindowTextW(GetDlgItem(g_main_window, IDC_STORAGE_COUNT), count_text);
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

static int copy_host_path(const wchar_t *host_path, const char *parent_dest);

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


static int apply_host_metadata(const wchar_t *host_path, const char *dest_path)
{
    DWORD attrs = GetFileAttributesW(host_path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return 0;
    HANDLE handle = CreateFileW(host_path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING,
        (attrs & FILE_ATTRIBUTE_DIRECTORY) ? FILE_FLAG_BACKUP_SEMANTICS : 0,
        NULL);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    FILE_BASIC_INFO basic;
    int okay = GetFileInformationByHandleEx(handle, FileBasicInfo,
                                             &basic, sizeof(basic)) != 0;
    CloseHandle(handle);
    if (!okay) return 0;
    struct infs_time_update update;
    infilfs_windows_basic_to_time_update(&basic, &update);
    infs_status status = infs_set_portable_flags(
        &g_volume, dest_path,
        infilfs_windows_attributes_to_portable(basic.FileAttributes));
    if (status == INFS_STATUS_OK)
        status = infs_set_times(&g_volume, dest_path, &update);
    return status == INFS_STATUS_OK;
}

static int join_windows_path(const wchar_t *parent, const wchar_t *name,
                             wchar_t *out, size_t out_count)
{
    if (!parent || !name || !out || !out_count)
        return 0;
    int written = _snwprintf_s(out, out_count, _TRUNCATE,
                               L"%s\\%s", parent, name);
    return written > 0 && (size_t)written < out_count;
}

static int copy_windows_tree(const wchar_t *source, const wchar_t *destination)
{
    DWORD attrs = GetFileAttributesW(source);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        set_windows_error(L"Read source path", GetLastError());
        return 0;
    }

    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        if (!CopyFileW(source, destination, FALSE)) {
            set_windows_error(L"Copy file through Explorer bridge",
                              GetLastError());
            return 0;
        }
        return 1;
    }

    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        SetLastError(ERROR_NOT_SUPPORTED);
        set_windows_error(L"Copy reparse-point directory", GetLastError());
        return 0;
    }

    if (!CreateDirectoryW(destination, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        set_windows_error(L"Create folder through Explorer bridge",
                          GetLastError());
        return 0;
    }

    wchar_t pattern[32768];
    if (_snwprintf_s(pattern, sizeof(pattern) / sizeof(pattern[0]),
                     _TRUNCATE, L"%s\\*", source) < 0) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        set_windows_error(L"Build source folder path", GetLastError());
        return 0;
    }

    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileW(pattern, &data);
    if (search == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND)
            return 1;
        set_windows_error(L"Enumerate source folder", error);
        return 0;
    }

    int okay = 1;
    do {
        if (wcscmp(data.cFileName, L".") == 0 ||
            wcscmp(data.cFileName, L"..") == 0)
            continue;
        wchar_t child_source[32768];
        wchar_t child_destination[32768];
        if (!join_windows_path(source, data.cFileName,
                               child_source,
                               sizeof(child_source) / sizeof(child_source[0])) ||
            !join_windows_path(destination, data.cFileName,
                               child_destination,
                               sizeof(child_destination) /
                                   sizeof(child_destination[0])) ||
            !copy_windows_tree(child_source, child_destination)) {
            okay = 0;
            break;
        }
    } while (FindNextFileW(search, &data));
    DWORD error = GetLastError();
    FindClose(search);
    if (okay && error != ERROR_NO_MORE_FILES) {
        set_windows_error(L"Enumerate source folder", error);
        okay = 0;
    }
    return okay;
}

static int copy_host_path_through_bridge(const wchar_t *host_path)
{
    wchar_t root[MAX_PATH * 4u];
    if (!infs_windows_bridge_root(
            root, sizeof(root) / sizeof(root[0]))) {
        SetLastError(ERROR_NOT_READY);
        set_windows_error(L"Locate Explorer projection", GetLastError());
        return 0;
    }

    const wchar_t *base = host_basename(host_path);
    if (!base[0])
        return 0;

    wchar_t destination[32768];
    if (!join_windows_path(root, base, destination,
                           sizeof(destination) / sizeof(destination[0]))) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        set_windows_error(L"Build projected destination path", GetLastError());
        return 0;
    }

    wchar_t status_text[640];
    _snwprintf_s(status_text,
                 sizeof(status_text) / sizeof(status_text[0]), _TRUNCATE,
                 L"Copying %s through the Windows Explorer bridge ...", base);
    set_status(status_text);

    if (!copy_windows_tree(host_path, destination))
        return 0;

    infs_status status = infs_volume_sync(&g_volume);
    if (status != INFS_STATUS_OK) {
        set_status_code(L"Commit Explorer-bridge copy", status);
        return 0;
    }
    return 1;
}

static int copy_selected_host_path(const wchar_t *host_path)
{
    if (infs_windows_bridge_active())
        return copy_host_path_through_bridge(host_path);
    return copy_host_path(host_path, "/");
}

static int create_copy_staging_path(const char *dest_path,
                                    char staging[INFS_PATH_MAX + 1u],
                                    const struct infs_create_options *options)
{
    const char *slash = strrchr(dest_path, '/');
    if (!slash)
        return 0;
    size_t parent_len = (size_t)(slash - dest_path);

    for (unsigned attempt = 0; attempt < 32u; ++attempt) {
        unsigned long sequence =
            (unsigned long)InterlockedIncrement(&g_copy_sequence);
        int written;
        if (parent_len == 0u) {
            written = snprintf(staging, INFS_PATH_MAX + 1u,
                               "/.infs-copy-%08lx-%08lx.tmp",
                               (unsigned long)GetCurrentProcessId(),
                               sequence);
        } else {
            written = snprintf(staging, INFS_PATH_MAX + 1u,
                               "%.*s/.infs-copy-%08lx-%08lx.tmp",
                               (int)parent_len, dest_path,
                               (unsigned long)GetCurrentProcessId(),
                               sequence);
        }
        if (written <= 0 || written > (int)INFS_PATH_MAX)
            return 0;

        infs_status status = infs_create_file(&g_volume, staging, options);
        if (status == INFS_STATUS_OK)
            return 1;
        if (status != INFS_STATUS_ALREADY_EXISTS) {
            set_status_code(L"Create temporary destination file", status);
            return 0;
        }
    }
    set_status_code(L"Create temporary destination file",
                    INFS_STATUS_ALREADY_EXISTS);
    return 0;
}

static int copy_host_file(const wchar_t *host_path, const char *dest_path)
{
    HANDLE input = CreateFileW(host_path, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (input == INVALID_HANDLE_VALUE) {
        set_windows_error(L"Open source file", GetLastError());
        return 0;
    }

    struct infs_create_options options;
    memset(&options, 0, sizeof(options));
    options.posix_permissions = 0644u;
    options.portable_flags = INFS_ATTR_HIDDEN | INFS_ATTR_TEMPORARY;

    char staging[INFS_PATH_MAX + 1u];
    if (!create_copy_staging_path(dest_path, staging, &options)) {
        CloseHandle(input);
        return 0;
    }

    infs_status status = INFS_STATUS_OK;
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
            set_windows_error(L"Read source file", GetLastError());
            okay = 0;
            break;
        }
        if (!got)
            break;
        int64_t written = infs_write_file_buffered(
            &g_volume, staging, buffer, got, offset);
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
    /*
     * Data and namespace publication are one CoW transaction. Publishing the
     * temporary file before the rename doubled the durability cost for every
     * copied file without improving atomicity for sub-threshold transfers.
     */
    if (okay) {
        status = infs_rename(&g_volume, staging, dest_path);
        if (status != INFS_STATUS_OK) {
            set_status_code(L"Publish copied file", status);
            okay = 0;
        }
    }
    if (okay && !apply_host_metadata(host_path, dest_path)) {
        set_status(L"Could not preserve source file attributes or timestamps.");
        okay = 0;
    }
    if (okay) {
        status = infs_volume_sync(&g_volume);
        if (status != INFS_STATUS_OK) {
            set_status_code(L"Commit copied file", status);
            okay = 0;
        }
    }
    if (!okay) {
        (void)infs_unlink(&g_volume, staging);
        (void)infs_volume_sync(&g_volume);
    }
    return okay;
}

static int copy_host_path(const wchar_t *host_path, const char *parent_dest)
{
    DWORD attrs = GetFileAttributesW(host_path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        set_windows_error(L"Read source path", GetLastError());
        return 0;
    }
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
    if (okay && !apply_host_metadata(host_path, dest))
        okay = 0;
    return okay;
}

static void refresh_contents(void)
{
    HWND list = GetDlgItem(g_main_window, IDC_CONTENTS);
    ListView_DeleteAllItems(list);
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

        wchar_t *type = items[i].type == INFS_OBJECT_DIRECTORY ?
                        L"Folder" :
                        items[i].type == INFS_OBJECT_SYMLINK ?
                        L"Symbolic link" : L"File";
        LVITEMW item;
        memset(&item, 0, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = (int)i;
        item.pszText = name;
        item.iImage = items[i].type == INFS_OBJECT_DIRECTORY ?
                      g_icon_folder :
                      items[i].type == INFS_OBJECT_SYMLINK ?
                      g_icon_link : g_icon_file;
        int row = ListView_InsertItem(list, &item);
        if (row >= 0)
            ListView_SetItemText(list, row, 1, type);
    }
    infs_free_dir_items(items);

    wchar_t text[224];
    _snwprintf_s(text, sizeof(text) / sizeof(text[0]), _TRUNCATE,
                 L"Ready. %zu item%s in the root. Drag files or folders into this window to copy them.",
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
        wchar_t size_text[64];
        if (!format_capacity_wide(
                target->size_bytes, size_text,
                sizeof(size_text) / sizeof(size_text[0])))
            wcscpy_s(size_text, sizeof(size_text) / sizeof(size_text[0]),
                     L"0 B");
        if (target->is_image) {
            _snwprintf_s(prompt, sizeof(prompt) / sizeof(prompt[0]), _TRUNCATE,
                         L"FORMAT THIS IMAGE AS INFILTRATORFS?\n\nFile: %s\nSize: %s\n\nEverything currently in this image file will be destroyed.",
                         target->device_path, size_text);
        } else if (target->use_region) {
            _snwprintf_s(prompt, sizeof(prompt) / sizeof(prompt[0]), _TRUNCATE,
                         L"FORMAT DISK %lu PARTITION %lu AS INFILTRATORFS?\n\nSize: %s\n\nEverything currently in this partition will be destroyed. The raw storage view is bounded to this partition.",
                         (unsigned long)target->disk_number,
                         (unsigned long)target->partition_number,
                         size_text);
        } else {
            _snwprintf_s(prompt, sizeof(prompt) / sizeof(prompt[0]), _TRUNCATE,
                         L"FORMAT THIS VOLUME AS INFILTRATORFS?\n\nLocation: %s\nSize: %s\n\nEverything currently on this volume will be destroyed. Windows will be locked out of the selected volume before writing.",
                         target->mount_point[0] ? target->mount_point : L"No drive letter",
                         size_text);
        }
        if (MessageBoxW(g_main_window, prompt, L"Confirm destructive format",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES)
            return 0;
    }

    close_volume();
    refresh_contents();
    update_buttons();
    struct infs_storage storage = {0};
    set_status(target->is_image ?
               L"Opening InfiltratorFS image ..." :
               (target->use_region ?
                L"Opening bounded raw partition ..." :
                L"Locking selected Windows volume ..."));
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
        okay = copy_selected_host_path(first);
    } else {
        wchar_t directory[32768];
        wcsncpy_s(directory, sizeof(directory) / sizeof(directory[0]),
                  first, _TRUNCATE);
        while (*next) {
            wchar_t full[32768];
            if (_snwprintf_s(full, sizeof(full) / sizeof(full[0]), _TRUNCATE,
                             L"%s\\%s", directory, next) < 0 ||
                !copy_selected_host_path(full)) {
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
    if (SHGetPathFromIDListW(item, path) && copy_selected_host_path(path))
        refresh_contents();
    CoTaskMemFree(item);
}

static void mount_windows_drive(void)
{
    if (!g_volume_open)
        return;

    if (infs_windows_bridge_active()) {
        wchar_t root[MAX_PATH * 4u];
        if (infs_windows_bridge_root(
                root, sizeof(root) / sizeof(root[0])))
            ShellExecuteW(g_main_window, L"open", root,
                          NULL, NULL, SW_SHOWNORMAL);
        return;
    }

    wchar_t drive[3] = {0};
    set_status(L"Starting driverless Windows Explorer bridge ...");
    if (!infs_windows_bridge_start(&g_volume, g_main_window,
                                   drive,
                                   sizeof(drive) / sizeof(drive[0]))) {
        update_buttons();
        return;
    }

    wchar_t root[MAX_PATH * 4u] = {0};
    infs_windows_bridge_root(root, sizeof(root) / sizeof(root[0]));
    wchar_t message[768];
    if (drive[0]) {
        _snwprintf_s(message, sizeof(message) / sizeof(message[0]), _TRUNCATE,
                     L"Explorer projection active. Projected folder: %s  \u2022  "
                     L"auxiliary drive alias: %s\\. Explorer is opened on "
                     L"the projected folder so UAC drive-namespace separation "
                     L"cannot hide the filesystem.",
                     root, drive);
    } else {
        _snwprintf_s(message, sizeof(message) / sizeof(message[0]), _TRUNCATE,
                     L"Explorer projection active at %s. Windows did not "
                     L"create a usable auxiliary drive alias, so Explorer is "
                     L"using the projected folder directly.",
                     root);
    }
    set_status(message);
    update_buttons();
}

static void unmount_windows_drive(void)
{
    if (!infs_windows_bridge_active())
        return;
    set_status(L"Flushing and unmounting Windows bridge ...");
    infs_windows_bridge_stop();
    refresh_contents();
    set_status(L"Windows bridge unmounted and InfiltratorFS changes flushed.");
    update_buttons();
}

static int ensure_volume_open(void)
{
    if (g_volume_open)
        return 1;
    return open_selected_volume(0);
}

static int maintenance_ready(void)
{
    if (infs_windows_bridge_active()) {
        MessageBoxW(
            g_main_window,
            L"Unmount the volume before running inspect, check, scrub or forensic maintenance.",
            L"InfiltratorFS", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    return ensure_volume_open();
}

static void check_volume(void)
{
    if (!maintenance_ready())
        return;
    struct infs_check_report report;
    memset(&report, 0, sizeof(report));
    set_status(L"Checking InfiltratorFS structure ...");
    infs_status status = infs_check(&g_volume, &report);
    if (status != INFS_STATUS_OK) {
        set_status_code(L"Check filesystem", status);
        return;
    }
    wchar_t message[640];
    _snwprintf_s(
        message, sizeof(message) / sizeof(message[0]), _TRUNCATE,
        L"Structural check complete.\n\n"
        L"Generation: %llu\nValid checkpoints: %u\n"
        L"Object index: %s\nAllocation ownership: %s\n"
        L"Namespace: %s\nChecksum metadata: %s",
        (unsigned long long)report.check_generation,
        (unsigned)report.checkpoint_replicas_valid,
        report.object_index_valid ? L"valid" : L"invalid",
        report.allocation_ownership_valid ? L"valid" : L"invalid",
        report.namespace_valid ? L"valid" : L"invalid",
        report.checksum_metadata_valid ? L"valid" : L"invalid");
    {
        const struct infilfs_manager_action_descriptor *action =
            infilfs_manager_action(INFILFS_MANAGER_ACTION_CHECK);
        if (action) {
            wchar_t success[256];
            if (utf8_to_wide_text(
                    action->success, success,
                    sizeof(success) / sizeof(success[0])))
                set_status(success);
        }
    }
    MessageBoxW(g_main_window, message, L"InfiltratorFS Check",
                MB_OK | MB_ICONINFORMATION);
}

static infs_status forensic_ignore_record(
    const struct infs_forensic_record *record, void *context)
{
    (void)record;
    (void)context;
    return INFS_STATUS_OK;
}

static void forensic_volume(void)
{
    if (!maintenance_ready())
        return;
    struct infs_forensic_summary summary;
    memset(&summary, 0, sizeof(summary));
    set_status(L"Scanning InfiltratorFS metadata ...");
    infs_status status = infs_forensic_scan(
        &g_volume.storage, forensic_ignore_record, NULL, &summary);
    if (status != INFS_STATUS_OK) {
        set_status_code(L"Forensic scan", status);
        return;
    }
    wchar_t message[768];
    _snwprintf_s(
        message, sizeof(message) / sizeof(message[0]), _TRUNCATE,
        L"Forensic scan complete.\n\n"
        L"Blocks scanned: %llu / %llu\nRecords found: %llu\n"
        L"Current: %llu\nStale: %llu\nOrphaned: %llu\nUnknown: %llu\n"
        L"Checkpoints: %llu\nObjects: %llu",
        (unsigned long long)summary.scanned_blocks,
        (unsigned long long)summary.total_blocks,
        (unsigned long long)summary.records_found,
        (unsigned long long)summary.current_records,
        (unsigned long long)summary.stale_records,
        (unsigned long long)summary.orphaned_records,
        (unsigned long long)summary.unknown_records,
        (unsigned long long)summary.checkpoints_found,
        (unsigned long long)summary.objects_found);
    {
        const struct infilfs_manager_action_descriptor *action =
            infilfs_manager_action(INFILFS_MANAGER_ACTION_FORENSIC);
        if (action) {
            wchar_t success[256];
            if (utf8_to_wide_text(
                    action->success, success,
                    sizeof(success) / sizeof(success[0])))
                set_status(success);
        }
    }
    MessageBoxW(g_main_window, message, L"InfiltratorFS Forensic Scan",
                MB_OK | MB_ICONINFORMATION);
}

static void inspect_volume(void)
{
    if (!maintenance_ready())
        return;

    wchar_t label[INFS_LABEL_MAX + 1u] = L"InfiltratorFS";
    char utf8_label[INFS_LABEL_MAX + 1u];
    memcpy(utf8_label, g_volume.sb.label, INFS_LABEL_MAX);
    utf8_label[INFS_LABEL_MAX] = '\0';
    MultiByteToWideChar(CP_UTF8, 0, utf8_label, -1,
                        label, (int)(sizeof(label) / sizeof(label[0])));

    uint64_t generation = infs_le64_to_cpu(g_volume.sb.generation);
    uint64_t total_blocks = infs_le64_to_cpu(g_volume.sb.total_blocks);
    uint64_t free_blocks = infs_le64_to_cpu(g_volume.sb.free_blocks);
    uint64_t used_blocks = total_blocks >= free_blocks ?
                           total_blocks - free_blocks : 0;
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    wchar_t total_size[64];
    wchar_t free_size[64];
    if (!infiltratr_u64_multiply_checked(
            total_blocks, (uint64_t)INFS_BLOCK_SIZE, &total_bytes) ||
        !infiltratr_u64_multiply_checked(
            free_blocks, (uint64_t)INFS_BLOCK_SIZE, &free_bytes) ||
        !format_capacity_wide(
            total_bytes, total_size,
            sizeof(total_size) / sizeof(total_size[0])) ||
        !format_capacity_wide(
            free_bytes, free_size,
            sizeof(free_size) / sizeof(free_size[0]))) {
        set_status_code(L"Render filesystem capacity", INFS_STATUS_OVERFLOW);
        return;
    }

    wchar_t message[1024];
    _snwprintf_s(message, sizeof(message) / sizeof(message[0]), _TRUNCATE,
                 L"Volume label: %s\n"
                 L"Format: %u.%u\n"
                 L"Generation: %llu\n"
                 L"Block size: %u bytes\n"
                 L"Total blocks: %llu (%s)\n"
                 L"Used blocks: %llu\n"
                 L"Free blocks: %llu (%s)",
                 label,
                 (unsigned)infs_le16_to_cpu(g_volume.sb.format_major),
                 (unsigned)infs_le16_to_cpu(g_volume.sb.format_minor),
                 (unsigned long long)generation,
                 (unsigned)INFS_BLOCK_SIZE,
                 (unsigned long long)total_blocks, total_size,
                 (unsigned long long)used_blocks,
                 (unsigned long long)free_blocks, free_size);
    {
        const struct infilfs_manager_action_descriptor *action =
            infilfs_manager_action(INFILFS_MANAGER_ACTION_INSPECT);
        if (action) {
            wchar_t success[256];
            if (utf8_to_wide_text(
                    action->success, success,
                    sizeof(success) / sizeof(success[0])))
                set_status(success);
        }
    }
    MessageBoxW(g_main_window, message, L"InfiltratorFS Inspection",
                MB_OK | MB_ICONINFORMATION);
}

static void scrub_volume(void)
{
    if (!maintenance_ready())
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
    {
        const struct infilfs_manager_action_descriptor *action =
            infilfs_manager_action(INFILFS_MANAGER_ACTION_SCRUB);
        if (action) {
            wchar_t success[256];
            if (utf8_to_wide_text(
                    action->success, success,
                    sizeof(success) / sizeof(success[0])))
                set_status(success);
        }
    }
    MessageBoxW(g_main_window, message, L"InfiltratorFS Scrub",
                MB_OK | MB_ICONINFORMATION);
}

static void show_about(void)
{
    wchar_t message[1024];
    _snwprintf_s(message, sizeof(message) / sizeof(message[0]), _TRUNCATE,
                 L"InfiltratorFS Manager for Windows " INFILFS_VERSION_W
                 L"\n\nInfiltratorFS implementation " INFILFS_VERSION_W
                 L"\nDisk format: %u.%u\n\nSame InfiltratorFS Manager application contract as Linux, rendered through the native Win32 presentation/storage adapter. Windows discovery includes raw physical partitions that have no drive letter or Windows filesystem driver.\n\nDriverless Explorer bridge: Microsoft's inbox Projected File System (ProjFS) exposes an opened InfiltratorFS volume through a projected Explorer folder, with an auxiliary drive alias when Windows can expose one in the current UAC namespace. InfiltratorFS ships no custom Windows kernel driver in this mode.\n\nLicence: GPL-3.0-or-later\n\nExperimental filesystem \u2014 use backed-up or disposable media while testing.",
                 (unsigned)INFS_FORMAT_MAJOR,
                 (unsigned)INFS_FORMAT_MINOR);
    MessageBoxW(g_main_window, message, L"About InfiltratorFS",
                MB_OK | MB_ICONINFORMATION);
}

static HMENU create_main_menu(void)
{
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU theme = CreatePopupMenu();
    HMENU help = CreatePopupMenu();
    if (!menu || !file || !view || !theme || !help)
        return menu;
    AppendMenuW(file, MF_STRING, IDM_FILE_REFRESH, L"&Refresh Volumes\tF5");
    AppendMenuW(file, MF_STRING, IDM_FILE_OPEN, L"&Open Selected Volume");
    AppendMenuW(file, MF_STRING, IDM_FILE_OPEN_IMAGE, L"Open &Image...");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_FILE_EXIT, L"E&xit");
    AppendMenuW(theme, MF_STRING, IDM_VIEW_THEME_SYSTEM, L"&System");
    AppendMenuW(theme, MF_STRING, IDM_VIEW_THEME_DAY, L"&Day");
    AppendMenuW(theme, MF_STRING, IDM_VIEW_THEME_NIGHT, L"&Night");
    AppendMenuW(view, MF_POPUP, (UINT_PTR)theme, L"&Theme");
    AppendMenuW(help, MF_STRING, IDM_HELP_ABOUT, L"&About InfiltratorFS");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)view, L"&View");
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
        size_t characters = 0;
        size_t bytes = 0;
        if (!infiltratr_size_add_checked(
                (size_t)length, 1u, &characters) ||
            !infiltratr_size_multiply_checked(
                characters, sizeof(wchar_t), &bytes)) {
            okay = 0;
            break;
        }
        wchar_t *path = malloc(bytes);
        if (!path) {
            okay = 0;
            break;
        }
        DragQueryFileW(drop, i, path, length + 1u);
        if (!copy_selected_host_path(path))
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
        update_theme_menu(hwnd);

        const InfiltratrTypography *typography = infiltratr_typography();
        const struct infilfs_manager_copy *copy = infilfs_manager_copy();
        const struct infilfs_manager_action_descriptor *inspect =
            infilfs_manager_action(INFILFS_MANAGER_ACTION_INSPECT);
        const struct infilfs_manager_action_descriptor *check =
            infilfs_manager_action(INFILFS_MANAGER_ACTION_CHECK);
        const struct infilfs_manager_action_descriptor *scrub =
            infilfs_manager_action(INFILFS_MANAGER_ACTION_SCRUB);
        const struct infilfs_manager_action_descriptor *forensic =
            infilfs_manager_action(INFILFS_MANAGER_ACTION_FORENSIC);
        if (!typography || !copy || !inspect || !check || !scrub || !forensic)
            return -1;

        g_ui_font = create_common_font(
            hwnd, 10, typography->ui_family, typography->ui_regular_weight);
        g_title_font = create_common_font(
            hwnd, 22, typography->brand_family, typography->brand_weight);
        g_heading_font = create_common_font(
            hwnd, 12, typography->ui_family, typography->ui_bold_weight);
        g_activity_font = create_common_font(
            hwnd, 9, typography->ui_family, typography->ui_regular_weight);
        if (!g_ui_font || !g_title_font || !g_heading_font || !g_activity_font)
            return -1;

        create_static_utf8(hwnd, IDC_HEADER_TITLE, copy->app_title, 0);
        create_static_utf8(hwnd, IDC_HEADER_SUBTITLE, copy->subtitle, 0);
        create_button_utf8(hwnd, IDC_NEW_IMAGE, copy->new_image_button);
        create_button_utf8(hwnd, IDC_OPEN_IMAGE, copy->open_image_button);
        create_button_utf8(hwnd, IDC_REFRESH, copy->refresh_button);
        create_button_utf8(hwnd, IDC_THEME, copy->theme_button);
        create_button_utf8(hwnd, IDC_ABOUT, copy->about_button);

        create_static_utf8(hwnd, IDC_STORAGE_HEADING, copy->storage_heading, 0);
        HWND target_list = CreateWindowExW(
            0, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
            WS_BORDER | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            0, 0, 0, 0, hwnd, (HMENU)IDC_TARGET, NULL, NULL);
        create_static_utf8(hwnd, IDC_STORAGE_COUNT, "0 available partitions", 0);
        CreateWindowW(L"STATIC", L"", WS_CHILD | SS_LEFT | SS_NOPREFIX,
                      0, 0, 0, 0, hwnd, (HMENU)IDC_TARGET_SUMMARY, NULL, NULL);

        create_static_utf8(hwnd, IDC_HERO_TITLE, copy->empty_title, 0);
        create_static_utf8(hwnd, IDC_HERO_PATH, "", SS_NOPREFIX);
        create_static_utf8(
            hwnd, IDC_MOUNT_BADGE, copy->unmounted_status, SS_CENTER);
        create_button_utf8(hwnd, IDC_MOUNT_DRIVE, copy->mount_button);
        create_button_utf8(hwnd, IDC_UNMOUNT_DRIVE, copy->unmount_button);
        create_button_utf8(hwnd, IDC_PAGE_OVERVIEW, copy->overview_tab);
        create_button_utf8(hwnd, IDC_PAGE_FILES, copy->files_tab);

        create_static_utf8(hwnd, IDC_STAT_CAPACITY_CAPTION, copy->capacity_caption, 0);
        create_static_utf8(hwnd, IDC_STAT_CAPACITY, "—", 0);
        create_static_utf8(hwnd, IDC_STAT_FILESYSTEM_CAPTION, copy->filesystem_caption, 0);
        create_static_utf8(hwnd, IDC_STAT_FILESYSTEM, "—", 0);
        create_static_utf8(hwnd, IDC_STAT_STATUS_CAPTION, copy->status_caption, 0);
        create_static_utf8(hwnd, IDC_STAT_STATUS, "—", 0);

        create_static_utf8(hwnd, IDC_VOLUME_INFO_HEADING, copy->volume_information, 0);
        create_static_utf8(hwnd, IDC_DETAIL_TYPE_CAPTION, copy->device_type, 0);
        create_static_utf8(hwnd, IDC_DETAIL_TYPE, "—", SS_NOPREFIX);
        create_static_utf8(hwnd, IDC_DETAIL_PATH_CAPTION, copy->device_path, 0);
        create_static_utf8(hwnd, IDC_DETAIL_PATH, "—", SS_NOPREFIX);
        create_static_utf8(
            hwnd, IDC_DETAIL_FS_CAPTION, copy->filesystem_label, 0);
        create_static_utf8(hwnd, IDC_DETAIL_FS, "—", 0);
        create_static_utf8(hwnd, IDC_DETAIL_LABEL_CAPTION, copy->volume_label, 0);
        create_static_utf8(hwnd, IDC_DETAIL_LABEL, "—", 0);
        create_static_utf8(hwnd, IDC_DETAIL_MOUNT_CAPTION, copy->mount_state, 0);
        create_static_utf8(hwnd, IDC_DETAIL_MOUNT, "—", 0);
        create_static_utf8(hwnd, IDC_DETAIL_MOUNTPOINT_CAPTION, copy->mount_point, 0);
        create_static_utf8(hwnd, IDC_DETAIL_MOUNTPOINT, "—", SS_NOPREFIX);

        create_static_utf8(hwnd, IDC_MAINTENANCE_HEADING, copy->maintenance, 0);
        create_static_utf8(hwnd, IDC_INSPECT_TITLE, inspect->title, 0);
        create_static_utf8(hwnd, IDC_INSPECT_DESC, inspect->description, 0);
        create_button_utf8(hwnd, IDC_INSPECT, inspect->button);
        create_static_utf8(hwnd, IDC_CHECK_TITLE, check->title, 0);
        create_static_utf8(hwnd, IDC_CHECK_DESC, check->description, 0);
        create_button_utf8(hwnd, IDC_CHECK, check->button);
        create_static_utf8(hwnd, IDC_SCRUB_TITLE, scrub->title, 0);
        create_static_utf8(hwnd, IDC_SCRUB_DESC, scrub->description, 0);
        create_button_utf8(hwnd, IDC_SCRUB, scrub->button);
        create_static_utf8(hwnd, IDC_FORENSIC_TITLE, forensic->title, 0);
        create_static_utf8(hwnd, IDC_FORENSIC_DESC, forensic->description, 0);
        create_button_utf8(hwnd, IDC_FORENSIC, forensic->button);

        create_static_utf8(hwnd, IDC_DANGER_HEADING, copy->danger_title, 0);
        create_static_utf8(hwnd, IDC_DANGER_DESC, copy->danger_description, 0);
        create_static_utf8(hwnd, IDC_LABEL_CAPTION, copy->volume_label, 0);
        HWND label = CreateWindowExW(
            0, L"EDIT", L"InfiltratorFS",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)IDC_LABEL, NULL, NULL);
        HWND format = create_button_utf8(hwnd, IDC_FORMAT, copy->format_button);

        create_static_utf8(hwnd, IDC_CONTENTS_HEADING, "Volume contents", 0);
        create_static_utf8(hwnd, IDC_CONTENTS_HINT,
                           "Root directory · Drag files or folders here to copy them", 0);
        HWND add_files = create_button_utf8(hwnd, IDC_ADD_FILES, "Add Files...");
        HWND add_folder = create_button_utf8(hwnd, IDC_ADD_FOLDER, "Add Folder...");
        HWND list = CreateWindowExW(
            0, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 0, 0, hwnd, (HMENU)IDC_CONTENTS, NULL, NULL);
        ListView_SetExtendedListViewStyle(
            list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        ListView_SetBkColor(list, g_connection_color);
        ListView_SetTextBkColor(list, g_connection_color);
        ListView_SetTextColor(list, g_text_color);
        g_content_images = ImageList_Create(20, 20, ILC_COLOR32 | ILC_MASK, 3, 1);
        if (g_content_images) {
            g_icon_file = add_stock_icon(g_content_images, SIID_DOCNOASSOC);
            g_icon_folder = add_stock_icon(g_content_images, SIID_FOLDER);
            g_icon_link = add_stock_icon(g_content_images, SIID_LINK);
            ListView_SetImageList(list, g_content_images, LVSIL_SMALL);
        }
        LVCOLUMNW column;
        memset(&column, 0, sizeof(column));
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = L"Name";
        column.cx = 460;
        column.iSubItem = 0;
        ListView_InsertColumn(list, 0, &column);
        column.pszText = L"Type";
        column.cx = 120;
        column.iSubItem = 1;
        ListView_InsertColumn(list, 1, &column);

        create_static_utf8(hwnd, IDC_ACTIVITY_HEADING, copy->activity_heading, 0);
        HWND clear_activity = create_button_utf8(hwnd, IDC_ACTIVITY_CLEAR, copy->clear_button);
        HWND activity = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            0, 0, 0, 0, hwnd, (HMENU)IDC_ACTIVITY, NULL, NULL);
        wchar_t ready_status[64] = L"";
        (void)utf8_to_wide_text(
            copy->ready_status, ready_status,
            sizeof(ready_status) / sizeof(ready_status[0]));
        HWND status = CreateWindowExW(
            0, L"STATIC", ready_status,
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
            0, 0, 0, 0, hwnd, (HMENU)IDC_STATUS, NULL, NULL);

        const int normal_ids[] = {
            IDC_HEADER_SUBTITLE, IDC_NEW_IMAGE, IDC_OPEN_IMAGE, IDC_REFRESH,
            IDC_THEME, IDC_ABOUT, IDC_TARGET, IDC_STORAGE_COUNT,
            IDC_HERO_PATH, IDC_MOUNT_BADGE, IDC_MOUNT_DRIVE, IDC_UNMOUNT_DRIVE,
            IDC_PAGE_OVERVIEW, IDC_PAGE_FILES,
            IDC_STAT_CAPACITY_CAPTION, IDC_STAT_FILESYSTEM_CAPTION,
            IDC_STAT_STATUS_CAPTION, IDC_DETAIL_TYPE_CAPTION,
            IDC_DETAIL_TYPE, IDC_DETAIL_PATH_CAPTION, IDC_DETAIL_PATH,
            IDC_DETAIL_FS_CAPTION, IDC_DETAIL_FS, IDC_DETAIL_LABEL_CAPTION,
            IDC_DETAIL_LABEL, IDC_DETAIL_MOUNT_CAPTION, IDC_DETAIL_MOUNT,
            IDC_DETAIL_MOUNTPOINT_CAPTION, IDC_DETAIL_MOUNTPOINT,
            IDC_INSPECT_DESC, IDC_INSPECT, IDC_CHECK_DESC, IDC_CHECK,
            IDC_SCRUB_DESC, IDC_SCRUB, IDC_FORENSIC_DESC, IDC_FORENSIC,
            IDC_DANGER_DESC, IDC_LABEL_CAPTION, IDC_LABEL, IDC_FORMAT,
            IDC_CONTENTS_HINT, IDC_ADD_FILES, IDC_ADD_FOLDER, IDC_CONTENTS,
            IDC_ACTIVITY_CLEAR, IDC_STATUS
        };
        for (size_t i = 0; i < sizeof(normal_ids) / sizeof(normal_ids[0]); ++i)
            set_control_font(hwnd, normal_ids[i], g_ui_font);
        const int heading_ids[] = {
            IDC_STORAGE_HEADING, IDC_VOLUME_INFO_HEADING,
            IDC_MAINTENANCE_HEADING, IDC_DANGER_HEADING,
            IDC_CONTENTS_HEADING, IDC_ACTIVITY_HEADING,
            IDC_INSPECT_TITLE, IDC_CHECK_TITLE, IDC_SCRUB_TITLE,
            IDC_FORENSIC_TITLE, IDC_STAT_CAPACITY, IDC_STAT_FILESYSTEM,
            IDC_STAT_STATUS
        };
        for (size_t i = 0; i < sizeof(heading_ids) / sizeof(heading_ids[0]); ++i)
            set_control_font(hwnd, heading_ids[i], g_heading_font);
        set_control_font(hwnd, IDC_HEADER_TITLE, g_title_font);
        set_control_font(hwnd, IDC_HERO_TITLE, g_title_font);
        set_control_font(hwnd, IDC_ACTIVITY, g_activity_font);

        SendMessageW(target_list, LB_SETITEMHEIGHT, 0, 32);
        const int themed_ids[] = {
            IDC_TARGET, IDC_NEW_IMAGE, IDC_OPEN_IMAGE, IDC_REFRESH, IDC_THEME,
            IDC_ABOUT, IDC_LABEL, IDC_FORMAT, IDC_MOUNT_DRIVE,
            IDC_UNMOUNT_DRIVE, IDC_PAGE_OVERVIEW, IDC_PAGE_FILES,
            IDC_INSPECT, IDC_CHECK, IDC_SCRUB, IDC_FORENSIC,
            IDC_ADD_FILES, IDC_ADD_FOLDER, IDC_CONTENTS,
            IDC_ACTIVITY_CLEAR, IDC_ACTIVITY
        };
        for (size_t i = 0; i < sizeof(themed_ids) / sizeof(themed_ids[0]); ++i)
            theme_control(GetDlgItem(hwnd, themed_ids[i]));
        theme_control(ListView_GetHeader(list));
        apply_window_visual_theme(hwnd);

        (void)label; (void)format; (void)add_files; (void)add_folder;
        (void)clear_activity; (void)activity; (void)status;
        DragAcceptFiles(hwnd, TRUE);
        layout_controls(hwnd);
        refresh_volumes();
        update_buttons();
        show_page(0);
        return 0;
    }
    case WM_SIZE:
        layout_controls(hwnd);
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wparam;
        HWND control = (HWND)lparam;
        int id = GetDlgCtrlID(control);
        SetBkMode(dc, TRANSPARENT);
        COLORREF foreground = g_text_color;
        HBRUSH brush = g_background_brush;

        if (id == IDC_HEADER_SUBTITLE || id == IDC_HERO_PATH ||
            id == IDC_CONTENTS_HINT || id == IDC_STATUS ||
            id == IDC_STORAGE_COUNT)
            foreground = g_summary_color;
        else if (id == IDC_STORAGE_HEADING ||
                 id == IDC_VOLUME_INFO_HEADING ||
                 id == IDC_MAINTENANCE_HEADING ||
                 id == IDC_CONTENTS_HEADING ||
                 id == IDC_ACTIVITY_HEADING)
            foreground = g_heading_color;
        else if (id == IDC_DANGER_HEADING)
            foreground = g_fault_color;
        else if (id == IDC_STAT_CAPACITY_CAPTION ||
                 id == IDC_STAT_FILESYSTEM_CAPTION ||
                 id == IDC_STAT_STATUS_CAPTION)
            foreground = g_kicker_color;
        else if (id == IDC_STAT_STATUS || id == IDC_MOUNT_BADGE)
            foreground = g_status_state_color;
        else if (id == IDC_INSPECT_TITLE)
            foreground = g_info_color;
        else if (id == IDC_CHECK_TITLE)
            foreground = g_success_color;
        else if (id == IDC_SCRUB_TITLE)
            foreground = g_warning_color;
        else if (id == IDC_FORENSIC_TITLE)
            foreground = g_accent_color;
        else if (id == IDC_INSPECT_DESC || id == IDC_CHECK_DESC ||
                 id == IDC_SCRUB_DESC || id == IDC_FORENSIC_DESC ||
                 id == IDC_DANGER_DESC)
            foreground = g_note_color;
        else if (id == IDC_DETAIL_TYPE_CAPTION ||
                 id == IDC_DETAIL_PATH_CAPTION ||
                 id == IDC_DETAIL_FS_CAPTION ||
                 id == IDC_DETAIL_LABEL_CAPTION ||
                 id == IDC_DETAIL_MOUNT_CAPTION ||
                 id == IDC_DETAIL_MOUNTPOINT_CAPTION ||
                 id == IDC_LABEL_CAPTION)
            foreground = g_detail_color;
        else if (id == IDC_STAT_CAPACITY || id == IDC_STAT_FILESYSTEM)
            foreground = g_heading_color;

        if (id == IDC_STAT_CAPACITY_CAPTION || id == IDC_STAT_CAPACITY ||
            id == IDC_STAT_FILESYSTEM_CAPTION || id == IDC_STAT_FILESYSTEM ||
            id == IDC_STAT_STATUS_CAPTION || id == IDC_STAT_STATUS)
            brush = g_card_brush;

        SetTextColor(dc, foreground);
        return (LRESULT)brush;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wparam;
        SetTextColor(dc, g_text_color);
        SetBkColor(dc, g_connection_color);
        return (LRESULT)g_connection_brush;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *limits = (MINMAXINFO *)lparam;
        limits->ptMinTrackSize.x = 1120;
        limits->ptMinTrackSize.y = 720;
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_TARGET && HIWORD(wparam) == LBN_SELCHANGE) {
            if (infs_windows_bridge_active())
                infs_windows_bridge_stop();
            close_volume();
            refresh_contents();
            update_buttons();
            return 0;
        }
        switch (LOWORD(wparam)) {
        case IDC_REFRESH:
        case IDM_FILE_REFRESH: refresh_volumes(); return 0;
        case IDC_NEW_IMAGE: create_image_dialog(); return 0;
        case IDC_FORMAT: open_selected_volume(1); update_buttons(); return 0;
        case IDC_OPEN:
        case IDM_FILE_OPEN: open_selected_volume(0); update_buttons(); return 0;
        case IDC_OPEN_IMAGE:
        case IDM_FILE_OPEN_IMAGE: open_image_dialog(); return 0;
        case IDC_PAGE_OVERVIEW: show_page(0); return 0;
        case IDC_PAGE_FILES:
            if (ensure_volume_open()) {
                refresh_contents();
                show_page(1);
            }
            return 0;
        case IDC_INSPECT: inspect_volume(); return 0;
        case IDC_CHECK: check_volume(); return 0;
        case IDC_ACTIVITY_CLEAR:
            SetWindowTextW(GetDlgItem(hwnd, IDC_ACTIVITY), L"");
            return 0;
        case IDC_ADD_FILES: add_files_dialog(); return 0;
        case IDC_ADD_FOLDER: add_folder_dialog(); return 0;
        case IDC_SCRUB: scrub_volume(); return 0;
        case IDC_FORENSIC: forensic_volume(); return 0;
        case IDC_MOUNT_DRIVE:
            if (ensure_volume_open())
                mount_windows_drive();
            update_buttons();
            return 0;
        case IDC_UNMOUNT_DRIVE:
            unmount_windows_drive();
            update_buttons();
            return 0;
        case IDC_THEME:
            set_theme_mode(hwnd, infiltratr_theme_mode_next(g_theme_mode));
            return 0;
        case IDC_ABOUT: show_about(); return 0;
        case IDM_VIEW_THEME_SYSTEM:
            set_theme_mode(hwnd, INFILTRATR_THEME_SYSTEM); return 0;
        case IDM_VIEW_THEME_DAY:
            set_theme_mode(hwnd, INFILTRATR_THEME_DAY); return 0;
        case IDM_VIEW_THEME_NIGHT:
            set_theme_mode(hwnd, INFILTRATR_THEME_NIGHT); return 0;
        case IDM_HELP_ABOUT: show_about(); return 0;
        case IDM_FILE_EXIT: DestroyWindow(hwnd); return 0;
        default: break;
        }
        break;
    case WM_SETTINGCHANGE:
        if (g_theme_mode == INFILTRATR_THEME_SYSTEM)
            apply_current_theme(hwnd);
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
        if (g_title_font)
            DeleteObject(g_title_font);
        if (g_heading_font)
            DeleteObject(g_heading_font);
        if (g_ui_font)
            DeleteObject(g_ui_font);
        if (g_activity_font)
            DeleteObject(g_activity_font);
        if (g_content_images) {
            ImageList_Destroy(g_content_images);
            g_content_images = NULL;
        }
        g_title_font = g_heading_font = g_ui_font = g_activity_font = NULL;
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
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_theme_mode = load_theme_mode();
    initialise_visual_theme();
    INITCOMMONCONTROLSEX controls = {
        sizeof(INITCOMMONCONTROLSEX),
        ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES
    };
    InitCommonControlsEx(&controls);
    if (!register_project_fonts(instance)) {
        MessageBoxW(NULL,
                    L"InfiltratorFS could not load its bundled MB Corpo fonts.",
                    L"InfiltratorFS", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
    const wchar_t class_name[] = L"InfiltratorFSWindowsTransfer";
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = g_background_brush;
    wc.lpszClassName = class_name;
    if (!RegisterClassW(&wc)) {
        unregister_project_fonts();
        CoUninitialize();
        return 1;
    }
    HWND window = CreateWindowW(
        class_name, L"InfiltratorFS Manager \u2014 Windows " INFILFS_VERSION_W,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1240, 800,
        NULL, NULL, instance, NULL);
    if (!window) {
        unregister_project_fonts();
        CoUninitialize();
        return 1;
    }
    ShowWindow(window, show_command);
    UpdateWindow(window);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_connection_brush) DeleteObject(g_connection_brush);
    if (g_surface_brush) DeleteObject(g_surface_brush);
    if (g_card_brush) DeleteObject(g_card_brush);
    if (g_panel_brush) DeleteObject(g_panel_brush);
    if (g_background_brush) DeleteObject(g_background_brush);
    g_connection_brush = g_surface_brush = g_card_brush = NULL;
    g_panel_brush = g_background_brush = NULL;
    unregister_project_fonts();
    CoUninitialize();
    return (int)msg.wParam;
}
#endif
