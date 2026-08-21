// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef _WIN32
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include "infilfs/format_volume.h"
#include "infilfs/format.h"
#include "infilfs/status.h"
#include "infilfs/volume.h"
#include "infilfs/win32_io.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

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

static struct infs_volume g_volume;
static int g_volume_open = 0;
static HWND g_main_window = NULL;

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
    wchar_t detail[256];
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

static void update_buttons(void)
{
    EnableWindow(GetDlgItem(g_main_window, IDC_ADD_FILES), g_volume_open);
    EnableWindow(GetDlgItem(g_main_window, IDC_ADD_FOLDER), g_volume_open);
    EnableWindow(GetDlgItem(g_main_window, IDC_SCRUB), g_volume_open);
}

static wchar_t selected_drive_letter(void)
{
    HWND combo = GetDlgItem(g_main_window, IDC_TARGET);
    LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR)
        return 0;
    LRESULT data = SendMessageW(combo, CB_GETITEMDATA, (WPARAM)selected, 0);
    return data == CB_ERR ? 0 : (wchar_t)data;
}

static void refresh_drives(void)
{
    HWND combo = GetDlgItem(g_main_window, IDC_TARGET);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);

    wchar_t windows_dir[MAX_PATH] = {0};
    GetWindowsDirectoryW(windows_dir, MAX_PATH);
    wchar_t system_drive = windows_dir[0] ? (wchar_t)towupper(windows_dir[0]) : L'C';
    DWORD mask = GetLogicalDrives();
    int added = 0;
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
        if (!(mask & (1u << (letter - L'A'))) || letter == system_drive)
            continue;
        wchar_t root[] = {letter, L':', L'\\', 0};
        UINT type = GetDriveTypeW(root);
        if (type != DRIVE_REMOVABLE && type != DRIVE_FIXED)
            continue;
        wchar_t type_text[32];
        wcscpy_s(type_text, sizeof(type_text) / sizeof(type_text[0]),
                 type == DRIVE_REMOVABLE ? L"Removable" : L"Fixed");
        ULARGE_INTEGER total = {0};
        wchar_t display[128];
        if (GetDiskFreeSpaceExW(root, NULL, &total, NULL)) {
            double gib = (double)total.QuadPart / (1024.0 * 1024.0 * 1024.0);
            _snwprintf_s(display, sizeof(display) / sizeof(display[0]), _TRUNCATE,
                         L"%c:   %s   %.1f GiB", letter, type_text, gib);
        } else {
            _snwprintf_s(display, sizeof(display) / sizeof(display[0]), _TRUNCATE,
                         L"%c:   %s / RAW", letter, type_text);
        }
        LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)display);
        if (index != CB_ERR && index != CB_ERRSPACE) {
            SendMessageW(combo, CB_SETITEMDATA, (WPARAM)index, (LPARAM)letter);
            ++added;
        }
    }
    if (added)
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
    set_status(added ? L"Select a non-system drive. Format erases that selected volume only."
                     : L"No non-system fixed/removable drive letters were found.");
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

    struct infs_create_options options = {0};
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
        int64_t written = infs_write_file(&g_volume, dest_path, buffer, got, offset);
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
        MessageBoxW(g_main_window, L"A filename is too long or cannot be represented as UTF-8.",
                    L"InfiltratorFS", MB_OK | MB_ICONERROR);
        return 0;
    }
    char dest[INFS_PATH_MAX + 1u];
    if (!make_child_path(parent_dest, name, dest))
        return 0;

    wchar_t status_text[512];
    _snwprintf_s(status_text, sizeof(status_text) / sizeof(status_text[0]), _TRUNCATE,
                 L"Copying %s ...", base);
    set_status(status_text);

    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY))
        return copy_host_file(host_path, dest);

    struct infs_create_options options = {0};
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
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
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
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, items[i].name, -1,
                                 name, (int)(sizeof(name) / sizeof(name[0]))))
            continue;
        wchar_t display[512];
        _snwprintf_s(display, sizeof(display) / sizeof(display[0]), _TRUNCATE,
                     items[i].type == INFS_OBJECT_DIRECTORY ? L"[DIR]  %s" : L"       %s",
                     name);
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)display);
    }
    infs_free_dir_items(items);
    wchar_t text[128];
    _snwprintf_s(text, sizeof(text) / sizeof(text[0]), _TRUNCATE,
                 L"Ready. %zu item%s in the root. Drag files or folders onto this window to copy them.",
                 count, count == 1u ? L"" : L"s");
    set_status(text);
}

static int build_device_path(wchar_t letter, wchar_t path[8])
{
    if (letter < L'A' || letter > L'Z')
        return 0;
    path[0] = L'\\'; path[1] = L'\\'; path[2] = L'.'; path[3] = L'\\';
    path[4] = letter; path[5] = L':'; path[6] = 0;
    return 1;
}

static int open_selected_volume(int format_first)
{
    wchar_t letter = selected_drive_letter();
    if (!letter) {
        MessageBoxW(g_main_window, L"Select a target drive first.", L"InfiltratorFS",
                    MB_OK | MB_ICONWARNING);
        return 0;
    }
    wchar_t device[8];
    build_device_path(letter, device);

    if (format_first) {
        wchar_t prompt[512];
        _snwprintf_s(prompt, sizeof(prompt) / sizeof(prompt[0]), _TRUNCATE,
                     L"FORMAT %c: AS INFILTRATORFS?\n\nEverything currently on %c: will be destroyed.\n\nThis app will lock and dismount that volume before writing.",
                     letter, letter);
        if (MessageBoxW(g_main_window, prompt, L"Confirm destructive format",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES)
            return 0;
    }

    close_volume();
    update_buttons();
    struct infs_storage storage = {0};
    set_status(L"Locking selected Windows volume ...");
    infs_status status = infs_win32_storage_open(&storage, device, 1, 1);
    if (status != INFS_STATUS_OK) {
        set_status_code(L"Lock selected volume", status);
        return 0;
    }

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
        set_status(L"Formatting selected volume as InfiltratorFS Format 0.7 ...");
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
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT |
                   OFN_PATHMUSTEXIST;
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
        wcsncpy_s(directory, sizeof(directory) / sizeof(directory[0]), first, _TRUNCATE);
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
    MessageBoxW(g_main_window, message, L"InfiltratorFS Scrub", MB_OK | MB_ICONINFORMATION);
}

static void handle_drop(HDROP drop)
{
    if (!g_volume_open) {
        MessageBoxW(g_main_window, L"Format or open an InfiltratorFS volume first.",
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
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        CreateWindowW(L"STATIC", L"Target Windows volume:", WS_CHILD | WS_VISIBLE,
                      18, 18, 180, 22, hwnd, NULL, NULL, NULL);
        HWND combo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE |
                      CBS_DROPDOWNLIST | WS_VSCROLL, 18, 42, 330, 300,
                      hwnd, (HMENU)IDC_TARGET, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE,
                      360, 40, 90, 27, hwnd, (HMENU)IDC_REFRESH, NULL, NULL);
        CreateWindowW(L"STATIC", L"Volume label:", WS_CHILD | WS_VISIBLE,
                      470, 18, 120, 22, hwnd, NULL, NULL, NULL);
        HWND label = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"InfiltratorFS",
                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 470, 42, 255, 25,
                      hwnd, (HMENU)IDC_LABEL, NULL, NULL);
        CreateWindowW(L"BUTTON", L"FORMAT selected drive", WS_CHILD | WS_VISIBLE,
                      18, 82, 205, 34, hwnd, (HMENU)IDC_FORMAT, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Open existing InfiltratorFS", WS_CHILD | WS_VISIBLE,
                      235, 82, 215, 34, hwnd, (HMENU)IDC_OPEN, NULL, NULL);
        HWND add_files = CreateWindowW(L"BUTTON", L"Add Files...", WS_CHILD | WS_VISIBLE,
                      470, 82, 120, 34, hwnd, (HMENU)IDC_ADD_FILES, NULL, NULL);
        HWND add_folder = CreateWindowW(L"BUTTON", L"Add Folder...", WS_CHILD | WS_VISIBLE,
                      600, 82, 125, 34, hwnd, (HMENU)IDC_ADD_FOLDER, NULL, NULL);
        HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                      18, 145, 707, 330, hwnd, (HMENU)IDC_CONTENTS, NULL, NULL);
        HWND scrub = CreateWindowW(L"BUTTON", L"Scrub / Verify", WS_CHILD | WS_VISIBLE,
                      18, 486, 125, 30, hwnd, (HMENU)IDC_SCRUB, NULL, NULL);
        HWND status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                      155, 491, 570, 42, hwnd, (HMENU)IDC_STATUS, NULL, NULL);
        SendMessageW(combo, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(label, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(add_files, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(add_folder, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(list, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(scrub, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(status, WM_SETFONT, (WPARAM)font, TRUE);
        DragAcceptFiles(hwnd, TRUE);
        refresh_drives();
        update_buttons();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_REFRESH: refresh_drives(); return 0;
        case IDC_FORMAT: open_selected_volume(1); return 0;
        case IDC_OPEN: open_selected_volume(0); return 0;
        case IDC_ADD_FILES: add_files_dialog(); return 0;
        case IDC_ADD_FOLDER: add_folder_dialog(); return 0;
        case IDC_SCRUB: scrub_volume(); return 0;
        default: break;
        }
        break;
    case WM_DROPFILES:
        handle_drop((HDROP)wparam);
        return 0;
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
    HWND window = CreateWindowW(class_name, L"InfiltratorFS Windows Transfer 0.7.1",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 760, 570,
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
