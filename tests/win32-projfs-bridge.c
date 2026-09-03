// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef _WIN32
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

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
static const char edited_linux_payload[] = "windows-edited-linux-file\n";
static const char exported_payload[] = "infiltratorfs-export-move\n";
#define DIRTY_RANGE_FILE_SIZE (8u * 1024u * 1024u)
#define DIRTY_RANGE_OFFSET (4u * 1024u * 1024u)

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

static int write_windows_range(const wchar_t *path,
                               uint64_t offset,
                               const void *data, DWORD size)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE | GENERIC_READ,
                              FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    LARGE_INTEGER position;
    position.QuadPart = (LONGLONG)offset;
    DWORD written = 0;
    int okay = SetFilePointerEx(file, position, NULL, FILE_BEGIN) &&
               WriteFile(file, data, size, &written, NULL) &&
               written == size &&
               FlushFileBuffers(file);
    CloseHandle(file);
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

static int run_windows_client(const wchar_t *root_arg)
{
    if (!root_arg || !root_arg[0])
        return fail(L"Invalid bridge client root");

    wchar_t root[32768];
    size_t length = wcslen(root_arg);
    if (length + 2u > sizeof(root) / sizeof(root[0]))
        return fail(L"Bridge client root is too long");
    wcsncpy_s(root, sizeof(root) / sizeof(root[0]), root_arg, _TRUNCATE);
    if (length && root[length - 1u] != L'\\') {
        root[length++] = L'\\';
        root[length] = L'\0';
    }

    wchar_t path[32768];
    char data[4096];
    DWORD got = 0;

    _snwprintf_s(path, sizeof(path) / sizeof(path[0]), _TRUNCATE,
                 L"%lslinux-cross-platform.txt", root);
    memset(data, 0, sizeof(data));
    if (!read_windows_file(path, data, sizeof(data) - 1u, &got) ||
        got != 32u ||
        memcmp(data, "linux-to-windows-cross-platform\n", 32u) != 0)
        return fail(L"Linux-created file was not readable through Explorer bridge");

    /*
     * This is the core cross-environment use case: modify a file that was
     * created on Linux, forcing its ProjFS placeholder to become a full local
     * Windows file, then require the close-time write-through path to replace
     * the InfiltratorFS contents.
     */
    if (!write_windows_file(path, edited_linux_payload,
                            (DWORD)(sizeof(edited_linux_payload) - 1u)))
        return fail(L"Edit Linux-created file through Windows bridge");

    _snwprintf_s(path, sizeof(path) / sizeof(path[0]), _TRUNCATE,
                 L"%lsdirty-range.bin", root);
    uint8_t dirty[INFS_BLOCK_SIZE];
    memset(dirty, 0x5a, sizeof(dirty));
    if (!write_windows_range(path, DIRTY_RANGE_OFFSET,
                             dirty, (DWORD)sizeof(dirty)))
        return fail(L"Edit one 4 KiB range in an existing large file");

    _snwprintf_s(path, sizeof(path) / sizeof(path[0]), _TRUNCATE, L"%lswindows-dir", root);
    if (!CreateDirectoryW(path, NULL))
        return fail(L"CreateDirectory through bridge");

    wchar_t original[32768];
    wchar_t renamed[32768];
    wchar_t hardlink[32768];
    _snwprintf_s(original, sizeof(original) / sizeof(original[0]), _TRUNCATE,
                 L"%lswindows-dir\\created.txt", root);
    _snwprintf_s(renamed, sizeof(renamed) / sizeof(renamed[0]), _TRUNCATE,
                 L"%lswindows-dir\\renamed.txt", root);
    _snwprintf_s(hardlink, sizeof(hardlink) / sizeof(hardlink[0]), _TRUNCATE,
                 L"%lswindows-dir\\hardlink.txt", root);

    if (!write_windows_file(original, windows_payload,
                            (DWORD)(sizeof(windows_payload) - 1u)))
        return fail(L"Create/write file through bridge");
    if (!MoveFileW(original, renamed))
        return fail(L"Rename file through bridge");
    if (!CreateHardLinkW(hardlink, renamed, NULL))
        return fail(L"Create hard link through bridge");

    wchar_t tree[32768];
    wchar_t tree_child[32768];
    wchar_t tree_renamed[32768];
    wchar_t tree_renamed_child[32768];
    _snwprintf_s(tree, sizeof(tree) / sizeof(tree[0]), _TRUNCATE, L"%lstree", root);
    _snwprintf_s(tree_child, sizeof(tree_child) / sizeof(tree_child[0]), _TRUNCATE,
                 L"%lstree\\child.txt", root);
    _snwprintf_s(tree_renamed, sizeof(tree_renamed) / sizeof(tree_renamed[0]), _TRUNCATE,
                 L"%lstree-renamed", root);
    _snwprintf_s(tree_renamed_child, sizeof(tree_renamed_child) / sizeof(tree_renamed_child[0]), _TRUNCATE,
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

    /*
     * Reproduce Explorer's same-volume rename fast-path directly. The original
     * failure surfaced as ERROR_NOT_SUPPORTED (0x80070032) when a directory
     * that originated in InfiltratorFS was moved out of the projected root.
     */
    wchar_t export_tree[32768];
    wchar_t export_child[32768];
    _snwprintf_s(export_tree,
                 sizeof(export_tree) / sizeof(export_tree[0]), _TRUNCATE,
                 L"%lsexport-tree", root);
    _snwprintf_s(export_child,
                 sizeof(export_child) / sizeof(export_child[0]), _TRUNCATE,
                 L"%lsexport-tree\\payload.txt", root);

    memset(data, 0, sizeof(data));
    if (!read_windows_file(export_child, data, sizeof(data) - 1u, &got) ||
        got != sizeof(exported_payload) - 1u ||
        memcmp(data, exported_payload, sizeof(exported_payload) - 1u) != 0)
        return fail(L"Hydrate projected export directory before move-out");

    /*
     * Put the destination outside the virtualization root but on the same NTFS
     * volume. This reproduces Explorer's troublesome fast-path exactly instead
     * of accidentally testing an ordinary cross-volume copy.
     */
    wchar_t local_appdata[32768];
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
                         NULL, SHGFP_TYPE_CURRENT, local_appdata) != S_OK)
        return fail(L"Resolve same-volume move-out destination");

    wchar_t destination_parent[32768];
    wchar_t moved_tree[32768];
    wchar_t moved_child[32768];
    _snwprintf_s(destination_parent,
                 sizeof(destination_parent) / sizeof(destination_parent[0]),
                 _TRUNCATE, L"%ls\\InfiltratorFS-ProjFS-MoveOut-%lu",
                 local_appdata, (unsigned long)GetCurrentProcessId());
    if (!CreateDirectoryW(destination_parent, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return fail(L"Create ordinary Windows move-out destination");
    _snwprintf_s(moved_tree,
                 sizeof(moved_tree) / sizeof(moved_tree[0]), _TRUNCATE,
                 L"%ls\\export-tree", destination_parent);
    _snwprintf_s(moved_child,
                 sizeof(moved_child) / sizeof(moved_child[0]), _TRUNCATE,
                 L"%ls\\export-tree\\payload.txt", destination_parent);

    if (!MoveFileW(export_tree, moved_tree))
        return fail(L"Same-volume move of projected directory out of bridge");

    memset(data, 0, sizeof(data));
    if (!read_windows_file(moved_child, data, sizeof(data) - 1u, &got) ||
        got != sizeof(exported_payload) - 1u ||
        memcmp(data, exported_payload, sizeof(exported_payload) - 1u) != 0)
        return fail(L"Verify ordinary Windows copy after move-out");
    if (GetFileAttributesW(export_tree) != INVALID_FILE_ATTRIBUTES)
        return fail(L"Projected source remained after Shell move-out");

    if (!DeleteFileW(moved_child) ||
        !RemoveDirectoryW(moved_tree) ||
        !RemoveDirectoryW(destination_parent))
        return fail(L"Clean ordinary Windows move-out destination");

    wchar_t delete_path[32768];
    _snwprintf_s(delete_path, sizeof(delete_path) / sizeof(delete_path[0]), _TRUNCATE,
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
    if (!root || !root[0])
        return fail(L"Invalid bridge root for external client");

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

    if (infs_create_file(&volume, "/dirty-range.bin", NULL) !=
        INFS_STATUS_OK) {
        infs_volume_close(&volume);
        return fail(L"Create dirty-range qualification file");
    }
    uint8_t *seed = malloc(256u * 1024u);
    if (!seed) {
        infs_volume_close(&volume);
        return 1;
    }
    for (size_t i = 0; i < 256u * 1024u; ++i)
        seed[i] = (uint8_t)((i * 37u + 11u) & 0xffu);
    for (uint64_t offset = 0; offset < DIRTY_RANGE_FILE_SIZE;
         offset += 256u * 1024u) {
        int64_t written = infs_write_file_buffered(
            &volume, "/dirty-range.bin", seed, 256u * 1024u, offset);
        if (written != 256 * 1024) {
            free(seed);
            infs_volume_close(&volume);
            return fail(L"Seed dirty-range qualification file");
        }
    }
    free(seed);
    status = infs_volume_sync(&volume);
    if (status != INFS_STATUS_OK) {
        infs_volume_close(&volume);
        return fail(L"Publish dirty-range qualification file");
    }

    if (infs_mkdir(&volume, "/export-tree", NULL) != INFS_STATUS_OK ||
        infs_create_file(&volume, "/export-tree/payload.txt", NULL) !=
            INFS_STATUS_OK ||
        infs_write_file_buffered(
            &volume, "/export-tree/payload.txt", exported_payload,
            sizeof(exported_payload) - 1u, 0u) !=
            (int64_t)(sizeof(exported_payload) - 1u) ||
        infs_volume_sync(&volume) != INFS_STATUS_OK) {
        infs_volume_close(&volume);
        return fail(L"Seed projected move-out qualification tree");
    }

    wchar_t drive[3] = {0};
    if (!infs_windows_bridge_start(&volume, NULL, drive,
                                   sizeof(drive) / sizeof(drive[0]))) {
        infs_volume_close(&volume);
        fwprintf(stderr, L"Could not start ProjFS bridge.\n");
        return 1;
    }

    wchar_t root[32768] = {0};
    if (!infs_windows_bridge_root(
            root, sizeof(root) / sizeof(root[0]))) {
        infs_windows_bridge_stop();
        infs_volume_close(&volume);
        return fail(L"Resolve ProjFS virtualization root");
    }
    /*
     * Qualify the actual projected directory from a separate client process.
     * The manager is elevated for raw-device access while Explorer is normally
     * unelevated, so a DOS-device alias can live in a different UAC namespace.
     * The projection root must therefore work independently of the drive alias.
     */
    int client_status = run_external_client(root);
    struct infs_windows_bridge_stats bridge_stats;
    memset(&bridge_stats, 0, sizeof(bridge_stats));
    int have_stats = infs_windows_bridge_get_stats(&bridge_stats);
    infs_windows_bridge_stop();
    if (client_status != 0) {
        infs_volume_close(&volume);
        return client_status;
    }

    char data[4096];
    int persisted = 1;
    if (!read_portable_file(&volume, "/linux-cross-platform.txt",
                            data, sizeof(data), edited_linux_payload)) {
        fwprintf(stderr,
                 L"FAIL: Windows edit of Linux-created file did not persist.\n");
        persisted = 0;
    }
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

    uint8_t dirty_verify[INFS_BLOCK_SIZE];
    memset(dirty_verify, 0, sizeof(dirty_verify));
    int64_t dirty_got = infs_read_file(
        &volume, "/dirty-range.bin", dirty_verify,
        sizeof(dirty_verify), DIRTY_RANGE_OFFSET);
    if (dirty_got != (int64_t)sizeof(dirty_verify)) {
        infs_volume_close(&volume);
        return fail(L"Read dirty-range qualification edit");
    }
    for (size_t i = 0; i < sizeof(dirty_verify); ++i) {
        if (dirty_verify[i] != 0x5a) {
            infs_volume_close(&volume);
            return fail(L"Dirty-range edit contents were not preserved");
        }
    }
    if (!have_stats ||
        bridge_stats.bytes_written >= UINT64_C(1024) * 1024u) {
        fwprintf(stderr,
                 L"FAIL: bounded edit wrote back %llu bytes (expected < 1 MiB).\n",
                 (unsigned long long)bridge_stats.bytes_written);
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
    if (infs_lookup_path(&volume, "/export-tree", &lookup) !=
        INFS_STATUS_NOT_FOUND) {
        infs_volume_close(&volume);
        fwprintf(stderr,
                 L"FAIL: Shell move-out did not delete the InfiltratorFS source.\n");
        return 1;
    }

    status = infs_volume_sync(&volume);
    infs_volume_close(&volume);
    if (status != INFS_STATUS_OK) {
        fwprintf(stderr, L"FAIL: final bridge sync returned %d.\n",
                 (int)status);
        return 1;
    }

    wprintf(L"Windows ProjFS projected-root cross-platform read/write/rename/"
            L"hardlink/delete/move-out qualification: PASS\n");
    return 0;
}
#endif
