// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef _WIN32
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "infiltratorfs-windows-bridge.h"
#include "infilfs/status.h"
#include "infilfs/volume.h"
#include "infilfs/win32_io.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const char windows_payload[] = "windows-projfs-write\n";

static int fail(const wchar_t *message)
{
    fwprintf(stderr, L"FAIL: %ls (Win32=%lu)\n",
             message, (unsigned long)GetLastError());
    return 1;
}

static int write_windows_file(const wchar_t *path,
                              const void *data, DWORD size)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE | GENERIC_READ,
                              FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    DWORD written = 0;
    int okay = WriteFile(file, data, size, &written, NULL) &&
               written == size &&
               FlushFileBuffers(file);
    CloseHandle(file);
    return okay;
}

static int read_windows_file(const wchar_t *path,
                             void *data, DWORD capacity, DWORD *size_out)
{
    HANDLE file = CreateFileW(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    DWORD got = 0;
    int okay = ReadFile(file, data, capacity, &got, NULL);
    CloseHandle(file);
    if (okay && size_out)
        *size_out = got;
    return okay;
}

static int read_portable_file(struct infs_volume *volume, const char *path,
                              char *buffer, size_t capacity,
                              const char *expected)
{
    memset(buffer, 0, capacity);
    int64_t got = infs_read_file(volume, path, buffer,
                                 capacity ? capacity - 1u : 0u, 0u);
    if (got < 0)
        return 0;
    buffer[(size_t)got] = '\0';
    return strcmp(buffer, expected) == 0;
}

static int run_windows_client(const wchar_t *root)
{
    wchar_t path[1024];
    char data[4096];
    DWORD got = 0;

    _snwprintf_s(path, 1024, _TRUNCATE,
                 L"%lslinux-cross-platform.txt", root);
    memset(data, 0, sizeof(data));
    if (!read_windows_file(path, data, sizeof(data) - 1u, &got) ||
        got != 32u ||
        memcmp(data, "linux-to-windows-cross-platform\n", 32u) != 0)
        return fail(L"Linux-created file was not readable through Explorer bridge");

    _snwprintf_s(path, 1024, _TRUNCATE, L"%lswindows-dir", root);
    if (!CreateDirectoryW(path, NULL))
        return fail(L"CreateDirectory through bridge");

    wchar_t original[1024];
    wchar_t renamed[1024];
    wchar_t hardlink[1024];
    _snwprintf_s(original, 1024, _TRUNCATE,
                 L"%lswindows-dir\\created.txt", root);
    _snwprintf_s(renamed, 1024, _TRUNCATE,
                 L"%lswindows-dir\\renamed.txt", root);
    _snwprintf_s(hardlink, 1024, _TRUNCATE,
                 L"%lswindows-dir\\hardlink.txt", root);

    if (!write_windows_file(original, windows_payload,
                            (DWORD)(sizeof(windows_payload) - 1u)))
        return fail(L"Create/write file through bridge");
    if (!MoveFileW(original, renamed))
        return fail(L"Rename file through bridge");
    if (!CreateHardLinkW(hardlink, renamed, NULL))
        return fail(L"Create hard link through bridge");

    wchar_t tree[1024];
    wchar_t tree_child[1024];
    wchar_t tree_renamed[1024];
    wchar_t tree_renamed_child[1024];
    _snwprintf_s(tree, 1024, _TRUNCATE, L"%lstree", root);
    _snwprintf_s(tree_child, 1024, _TRUNCATE,
                 L"%lstree\\child.txt", root);
    _snwprintf_s(tree_renamed, 1024, _TRUNCATE,
                 L"%lstree-renamed", root);
    _snwprintf_s(tree_renamed_child, 1024, _TRUNCATE,
                 L"%lstree-renamed\\child.txt", root);

    if (!CreateDirectoryW(tree, NULL) ||
        !write_windows_file(tree_child, "before-rename\n", 14u))
        return fail(L"Create directory tree through bridge");

    memset(data, 0, sizeof(data));
    if (!read_windows_file(tree_child, data, sizeof(data) - 1u, &got) ||
        got != 14u)
        return fail(L"Hydrate child before directory rename");

    if (!MoveFileW(tree, tree_renamed) ||
        !write_windows_file(tree_renamed_child, "after-rename\n", 13u))
        return fail(L"Rename projected directory and rewrite child");

    wchar_t delete_path[1024];
    _snwprintf_s(delete_path, 1024, _TRUNCATE,
                 L"%lsdelete-me.txt", root);
    if (!write_windows_file(delete_path, "delete\n", 7u) ||
        !DeleteFileW(delete_path))
        return fail(L"Create/delete file through bridge");

    return 0;
}

static int run_external_client(const wchar_t *root)
{
    wchar_t executable[32768];
    DWORD length = GetModuleFileNameW(
        NULL, executable,
        (DWORD)(sizeof(executable) / sizeof(executable[0])));
    if (!length || length >= sizeof(executable) / sizeof(executable[0]))
        return fail(L"GetModuleFileName for bridge client");

    wchar_t command[65536];
    if (_snwprintf_s(command,
                     sizeof(command) / sizeof(command[0]), _TRUNCATE,
                     L"\"%ls\" --client \"%ls\"",
                     executable, root) < 0)
        return fail(L"Build bridge client command line");

    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);

    if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, 0,
                        NULL, NULL, &startup, &process))
        return fail(L"Start external Windows bridge client");

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    if (!GetExitCodeProcess(process.hProcess, &exit_code))
        exit_code = 1;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return (int)exit_code;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc == 3 && wcscmp(argv[1], L"--client") == 0)
        return run_windows_client(argv[2]);

    if (argc != 2) {
        fwprintf(stderr, L"Usage: %ls <writable-infiltratorfs-image>\n",
                 argv[0]);
        return 2;
    }

    HMODULE projfs = LoadLibraryW(L"ProjectedFSLib.dll");
    if (!projfs) {
        wprintf(L"SKIP: ProjectedFSLib.dll is not enabled on this Windows host.\n");
        return 0;
    }
    FreeLibrary(projfs);
    _wputenv_s(L"INFILTRATORFS_BRIDGE_NO_EXPLORER", L"1");
    _wputenv_s(L"INFILTRATORFS_BRIDGE_TRACE", L"1");

    struct infs_storage storage;
    memset(&storage, 0, sizeof(storage));
    infs_status status =
        infs_win32_storage_open(&storage, argv[1], 1, 0);
    if (status != INFS_STATUS_OK) {
        fwprintf(stderr, L"Could not open test image: %d\n", (int)status);
        return 1;
    }

    struct infs_volume volume;
    memset(&volume, 0, sizeof(volume));
    status = infs_volume_open_storage(&volume, &storage, 1);
    if (status != INFS_STATUS_OK) {
        infs_storage_close(&storage);
        fwprintf(stderr, L"Could not open InfiltratorFS volume: %d\n",
                 (int)status);
        return 1;
    }

    status = infs_volume_set_deferred_publish(
        &volume, 1, UINT64_C(16) * 1024u * 1024u);
    if (status != INFS_STATUS_OK) {
        infs_volume_close(&volume);
        return 1;
    }

    wchar_t drive[3] = {0};
    if (!infs_windows_bridge_start(&volume, NULL, drive,
                                   sizeof(drive) / sizeof(drive[0]))) {
        infs_volume_close(&volume);
        fwprintf(stderr, L"Could not start ProjFS bridge.\n");
        return 1;
    }

    wchar_t root[4] = {drive[0], L':', L'\\', L'\0'};
    int client_status = run_external_client(root);
    infs_windows_bridge_stop();
    if (client_status != 0) {
        infs_volume_close(&volume);
        return client_status;
    }

    char data[4096];
    int persisted = 1;
    if (!read_portable_file(&volume, "/windows-dir/renamed.txt",
                            data, sizeof(data), windows_payload)) {
        fwprintf(stderr,
                 L"FAIL: /windows-dir/renamed.txt did not persist expected data.\n");
        persisted = 0;
    }
    if (!read_portable_file(&volume, "/windows-dir/hardlink.txt",
                            data, sizeof(data), windows_payload)) {
        fwprintf(stderr,
                 L"FAIL: /windows-dir/hardlink.txt did not persist expected data.\n");
        persisted = 0;
    }
    if (!read_portable_file(&volume, "/tree-renamed/child.txt",
                            data, sizeof(data), "after-rename\n")) {
        fwprintf(stderr,
                 L"FAIL: /tree-renamed/child.txt did not persist expected data.\n");
        persisted = 0;
    }
    if (!persisted) {
        infs_volume_close(&volume);
        return 1;
    }

    struct infs_attributes first;
    struct infs_attributes second;
    if (infs_get_attributes(&volume, "/windows-dir/renamed.txt",
                            &first) != INFS_STATUS_OK ||
        infs_get_attributes(&volume, "/windows-dir/hardlink.txt",
                            &second) != INFS_STATUS_OK ||
        memcmp(first.object_id, second.object_id, 16u) != 0 ||
        first.link_count < 2u || second.link_count < 2u) {
        infs_volume_close(&volume);
        fwprintf(stderr, L"FAIL: hard-link identity was not preserved.\n");
        return 1;
    }

    struct infs_lookup lookup;
    if (infs_lookup_path(&volume, "/delete-me.txt", &lookup) !=
        INFS_STATUS_NOT_FOUND) {
        infs_volume_close(&volume);
        fwprintf(stderr, L"FAIL: deleted Windows file still exists.\n");
        return 1;
    }

    status = infs_volume_sync(&volume);
    infs_volume_close(&volume);
    if (status != INFS_STATUS_OK) {
        fwprintf(stderr, L"FAIL: final bridge sync returned %d.\n",
                 (int)status);
        return 1;
    }

    wprintf(L"Windows ProjFS bridge cross-platform read/write/rename/"
            L"hardlink/delete qualification: PASS\n");
    return 0;
}
#endif
