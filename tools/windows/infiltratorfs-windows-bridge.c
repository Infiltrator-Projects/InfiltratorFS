// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef _WIN32
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <projectedfslib.h>
#include <shellapi.h>
#include <shlobj.h>

#include "infiltratorfs-windows-bridge.h"
#include "infilfs/format.h"
#include "infilfs/status.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define BRIDGE_READ_CHUNK (4u * 1024u * 1024u)
#define BRIDGE_IDLE_FLUSH_MS 1200u
#define BRIDGE_PROVIDER_THREADS 4u

#define BRIDGE_NOTIFY_PERSIST_MASK ( \
    PRJ_NOTIFY_FILE_OVERWRITTEN | \
    PRJ_NOTIFY_FILE_HANDLE_CLOSED_FILE_MODIFIED | \
    PRJ_NOTIFY_FILE_HANDLE_CLOSED_FILE_DELETED | \
    PRJ_NOTIFY_FILE_RENAMED | \
    PRJ_NOTIFY_HARDLINK_CREATED | \
    PRJ_NOTIFY_FILE_PRE_CONVERT_TO_FULL)

#define BRIDGE_NOTIFY_ROOT_MASK ( \
    PRJ_NOTIFY_NEW_FILE_CREATED | BRIDGE_NOTIFY_PERSIST_MASK)

struct bridge_dir_entry {
    wchar_t name[INFS_NAME_MAX + 1u];
    struct infs_attributes attributes;
};

struct bridge_alias {
    wchar_t from[INFS_PATH_MAX + 1u];
    wchar_t to[INFS_PATH_MAX + 1u];
    struct bridge_alias *next;
};

struct bridge_identity {
    uint8_t token[16];
    uint8_t object_id[16];
    wchar_t relative_path[INFS_PATH_MAX + 1u];
    struct bridge_identity *next;
};

struct bridge_enum {
    GUID id;
    wchar_t relative_path[INFS_PATH_MAX + 1u];
    wchar_t search[INFS_NAME_MAX + 1u];
    struct bridge_dir_entry *entries;
    size_t count;
    size_t index;
    struct bridge_enum *next;
};

struct bridge_state {
    struct infs_volume *volume;
    HWND owner;
    PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT context;
    wchar_t root[MAX_PATH * 4u];
    wchar_t dos_target[MAX_PATH * 4u + 8u];
    wchar_t drive[3];
    CRITICAL_SECTION lock;
    int lock_ready;
    int active;
    HANDLE flush_timer;
    ULONGLONG last_mutation_tick;
    int publish_pending;
    struct infs_windows_bridge_stats stats;
    struct bridge_enum *enums;
    struct bridge_alias *aliases;
    struct bridge_identity *identities;
};

static struct bridge_state g_bridge;

static infs_status bridge_publish_locked(void)
{
    if (!g_bridge.publish_pending || !g_bridge.volume)
        return INFS_STATUS_OK;

    infs_status status = infs_volume_sync(g_bridge.volume);
    if (status == INFS_STATUS_OK) {
        g_bridge.publish_pending = 0;
        g_bridge.stats.publish_count++;
    }
    return status;
}

static infs_status bridge_note_mutation_locked(void)
{
    g_bridge.publish_pending = 1;
    g_bridge.last_mutation_tick = GetTickCount64();

    /*
     * The timer normally coalesces a burst of Explorer operations into one
     * checkpoint publication. If timer creation was unavailable, preserve the
     * old durability behaviour rather than leaving mutations pending.
     */
    if (!g_bridge.flush_timer)
        return bridge_publish_locked();
    return INFS_STATUS_OK;
}

static VOID CALLBACK bridge_idle_flush(PVOID context, BOOLEAN fired)
{
    (void)context;
    (void)fired;
    if (!g_bridge.lock_ready)
        return;

    EnterCriticalSection(&g_bridge.lock);
    if (g_bridge.active && g_bridge.publish_pending) {
        ULONGLONG now = GetTickCount64();
        if (now - g_bridge.last_mutation_tick >= BRIDGE_IDLE_FLUSH_MS)
            (void)bridge_publish_locked();
    }
    LeaveCriticalSection(&g_bridge.lock);
}

typedef HRESULT (WINAPI *bridge_prj_mark_directory_fn)(
    PCWSTR, PCWSTR, const PRJ_PLACEHOLDER_VERSION_INFO *, const GUID *);
typedef HRESULT (WINAPI *bridge_prj_start_fn)(
    PCWSTR, const PRJ_CALLBACKS *, const void *,
    const PRJ_STARTVIRTUALIZING_OPTIONS *,
    PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT *);
typedef void (WINAPI *bridge_prj_stop_fn)(
    PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT);
typedef INT (WINAPI *bridge_prj_name_compare_fn)(PCWSTR, PCWSTR);
typedef BOOLEAN (WINAPI *bridge_prj_name_match_fn)(PCWSTR, PCWSTR);
typedef HRESULT (WINAPI *bridge_prj_fill_dir_fn)(
    PCWSTR, const PRJ_FILE_BASIC_INFO *, PRJ_DIR_ENTRY_BUFFER_HANDLE);
typedef HRESULT (WINAPI *bridge_prj_write_placeholder_fn)(
    PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT, PCWSTR,
    const PRJ_PLACEHOLDER_INFO *, UINT32);
typedef void *(WINAPI *bridge_prj_alloc_fn)(
    PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT, size_t);
typedef void (WINAPI *bridge_prj_free_fn)(void *);
typedef HRESULT (WINAPI *bridge_prj_write_data_fn)(
    PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT, const GUID *, const void *,
    UINT64, UINT32);

struct bridge_projfs_api {
    HMODULE module;
    bridge_prj_mark_directory_fn mark_directory;
    bridge_prj_start_fn start;
    bridge_prj_stop_fn stop;
    bridge_prj_name_compare_fn name_compare;
    bridge_prj_name_match_fn name_match;
    bridge_prj_fill_dir_fn fill_dir;
    bridge_prj_write_placeholder_fn write_placeholder;
    bridge_prj_alloc_fn alloc;
    bridge_prj_free_fn free_buffer;
    bridge_prj_write_data_fn write_data;
};

static struct bridge_projfs_api g_projfs;

static int bridge_alias_prefix_match(PCWSTR path, PCWSTR prefix);
static int bridge_resolve_relative(PCWSTR relative,
                                   wchar_t out[INFS_PATH_MAX + 1u]);

static const uint8_t bridge_provider_id[] = {
    'I','N','F','S','-','P','R','O','J','F','S','-','V','1'
};

static int bridge_load_proc(FARPROC *out, const char *name)
{
    *out = GetProcAddress(g_projfs.module, name);
    return *out != NULL;
}

static int bridge_load_projfs_api(void)
{
    if (g_projfs.module)
        return 1;

    HMODULE module = LoadLibraryW(L"ProjectedFSLib.dll");
    if (!module)
        return 0;
    memset(&g_projfs, 0, sizeof(g_projfs));
    g_projfs.module = module;

#define LOAD_PRJ(member, name) do { \
    FARPROC proc = NULL; \
    if (!bridge_load_proc(&proc, name)) { \
        FreeLibrary(g_projfs.module); \
        memset(&g_projfs, 0, sizeof(g_projfs)); \
        return 0; \
    } \
    memcpy(&g_projfs.member, &proc, sizeof(g_projfs.member)); \
} while (0)

    LOAD_PRJ(mark_directory, "PrjMarkDirectoryAsPlaceholder");
    LOAD_PRJ(start, "PrjStartVirtualizing");
    LOAD_PRJ(stop, "PrjStopVirtualizing");
    LOAD_PRJ(name_compare, "PrjFileNameCompare");
    LOAD_PRJ(name_match, "PrjFileNameMatch");
    LOAD_PRJ(fill_dir, "PrjFillDirEntryBuffer");
    LOAD_PRJ(write_placeholder, "PrjWritePlaceholderInfo");
    LOAD_PRJ(alloc, "PrjAllocateAlignedBuffer");
    LOAD_PRJ(free_buffer, "PrjFreeAlignedBuffer");
    LOAD_PRJ(write_data, "PrjWriteFileData");
#undef LOAD_PRJ
    return 1;
}

static DWORD bridge_enable_projfs_feature(void)
{
    wchar_t command[] =
        L"dism.exe /Online /Enable-Feature /FeatureName:Client-ProjFS "
        L"/NoRestart /Quiet";
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL,
                        &startup, &process))
        return GetLastError();

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = ERROR_GEN_FAILURE;
    if (!GetExitCodeProcess(process.hProcess, &exit_code))
        exit_code = GetLastError();
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code;
}

static int bridge_require_projfs(HWND owner)
{
    if (bridge_load_projfs_api())
        return 1;

    int answer = MessageBoxW(
        owner,
        L"The Microsoft Windows Projected File System (ProjFS) optional "
        L"component is not enabled.\n\n"
        L"InfiltratorFS can enable this built-in Microsoft component now. "
        L"No InfiltratorFS kernel driver or driver signing is involved.\n\n"
        L"Enable Windows Projected File System?",
        L"InfiltratorFS Windows Bridge",
        MB_YESNO | MB_DEFBUTTON1 | MB_ICONINFORMATION);
    if (answer != IDYES)
        return 0;

    DWORD result = bridge_enable_projfs_feature();
    if (result != ERROR_SUCCESS &&
        result != ERROR_SUCCESS_REBOOT_REQUIRED) {
        wchar_t message[384];
        _snwprintf_s(message,
                     sizeof(message) / sizeof(message[0]), _TRUNCATE,
                     L"Windows could not enable Client-ProjFS "
                     L"(DISM exit code %lu).",
                     (unsigned long)result);
        MessageBoxW(owner, message, L"InfiltratorFS Windows Bridge",
                    MB_OK | MB_ICONERROR);
        return 0;
    }

    if (result == ERROR_SUCCESS_REBOOT_REQUIRED ||
        !bridge_load_projfs_api()) {
        MessageBoxW(
            owner,
            L"Windows Projected File System has been enabled, but Windows "
            L"must be restarted before the InfiltratorFS Explorer bridge "
            L"can be used.",
            L"InfiltratorFS Windows Bridge",
            MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    return 1;
}

static int bridge_version_is_ours(const PRJ_PLACEHOLDER_VERSION_INFO *version)
{
    if (!version)
        return 0;
    return memcmp(version->ProviderID, bridge_provider_id,
                  sizeof(bridge_provider_id)) == 0;
}

static void bridge_fill_version(
    const struct infs_attributes *attributes, const uint8_t token[16],
    PRJ_PLACEHOLDER_VERSION_INFO *version)
{
    memset(version, 0, sizeof(*version));
    memcpy(version->ProviderID, bridge_provider_id,
           sizeof(bridge_provider_id));
    memcpy(version->ContentID, token, 16u);
    memcpy(version->ContentID + 16u, &attributes->change_time_ns,
           sizeof(attributes->change_time_ns));
    memcpy(version->ContentID + 24u, &attributes->logical_size,
           sizeof(attributes->logical_size));
}

static struct bridge_identity *bridge_find_identity_by_token(
    const uint8_t token[16])
{
    for (struct bridge_identity *identity = g_bridge.identities;
         identity; identity = identity->next) {
        if (memcmp(identity->token, token, 16u) == 0)
            return identity;
    }
    return NULL;
}

static struct bridge_identity *bridge_find_identity_by_path(
    PCWSTR relative_path)
{
    if (!relative_path)
        return NULL;
    for (struct bridge_identity *identity = g_bridge.identities;
         identity; identity = identity->next) {
        if (_wcsicmp(identity->relative_path, relative_path) == 0)
            return identity;
    }
    return NULL;
}

static struct bridge_identity *bridge_remember_identity(
    const uint8_t object_id[16], PCWSTR relative_path)
{
    if (!relative_path || wcslen(relative_path) > INFS_PATH_MAX)
        return NULL;

    struct bridge_identity *identity =
        bridge_find_identity_by_path(relative_path);
    if (!identity) {
        identity = calloc(1, sizeof(*identity));
        if (!identity)
            return NULL;
        GUID token;
        if (FAILED(CoCreateGuid(&token))) {
            free(identity);
            return NULL;
        }
        memcpy(identity->token, &token, 16u);
        identity->next = g_bridge.identities;
        g_bridge.identities = identity;
    }
    memcpy(identity->object_id, object_id, 16u);
    wcsncpy_s(identity->relative_path, INFS_PATH_MAX + 1u,
              relative_path, _TRUNCATE);
    return identity;
}

static int bridge_identity_relative(
    const PRJ_CALLBACK_DATA *callback_data,
    wchar_t out[INFS_PATH_MAX + 1u])
{
    if (callback_data && bridge_version_is_ours(callback_data->VersionInfo)) {
        struct bridge_identity *identity =
            bridge_find_identity_by_token(callback_data->VersionInfo->ContentID);
        if (identity) {
            wcsncpy_s(out, INFS_PATH_MAX + 1u,
                      identity->relative_path, _TRUNCATE);
            return 1;
        }
    }
    if (!callback_data || !callback_data->FilePathName ||
        wcslen(callback_data->FilePathName) > INFS_PATH_MAX)
        return 0;
    wcsncpy_s(out, INFS_PATH_MAX + 1u,
              callback_data->FilePathName, _TRUNCATE);
    return 1;
}

static void bridge_update_identity_prefix(PCWSTR old_path, PCWSTR new_path)
{
    if (!old_path || !*old_path || !new_path || !*new_path)
        return;
    size_t old_length = wcslen(old_path);
    for (struct bridge_identity *identity = g_bridge.identities;
         identity; identity = identity->next) {
        if (!bridge_alias_prefix_match(identity->relative_path, old_path))
            continue;
        PCWSTR suffix = identity->relative_path + old_length;
        wchar_t rewritten[INFS_PATH_MAX + 1u];
        if (_snwprintf_s(rewritten,
                         sizeof(rewritten) / sizeof(rewritten[0]),
                         _TRUNCATE, L"%s%s", new_path, suffix) < 0)
            continue;
        wcscpy_s(identity->relative_path, INFS_PATH_MAX + 1u, rewritten);
    }
}

static void bridge_forget_identity_prefix(PCWSTR path)
{
    if (!path || !*path)
        return;
    struct bridge_identity **link = &g_bridge.identities;
    while (*link) {
        if (bridge_alias_prefix_match((*link)->relative_path, path)) {
            struct bridge_identity *victim = *link;
            *link = victim->next;
            free(victim);
            continue;
        }
        link = &(*link)->next;
    }
}

static INT64 unix_ns_to_filetime(int64_t ns)
{
    const int64_t windows_epoch_seconds = INT64_C(11644473600);
    int64_t seconds = ns / INT64_C(1000000000);
    int64_t remainder = ns % INT64_C(1000000000);
    if (remainder < 0) {
        remainder += INT64_C(1000000000);
        --seconds;
    }
    if (seconds < -windows_epoch_seconds)
        return 0;
    return (seconds + windows_epoch_seconds) * INT64_C(10000000) +
           remainder / INT64_C(100);
}

static void attributes_to_basic(const struct infs_attributes *attributes,
                                PRJ_FILE_BASIC_INFO *basic)
{
    memset(basic, 0, sizeof(*basic));
    basic->IsDirectory =
        attributes->object_type == INFS_OBJECT_DIRECTORY ? TRUE : FALSE;
    basic->FileSize =
        attributes->object_type == INFS_OBJECT_DIRECTORY ? 0 :
        (INT64)attributes->logical_size;
    basic->CreationTime.QuadPart = unix_ns_to_filetime(attributes->birth_time_ns);
    basic->LastAccessTime.QuadPart = unix_ns_to_filetime(attributes->access_time_ns);
    basic->LastWriteTime.QuadPart =
        unix_ns_to_filetime(attributes->modification_time_ns);
    basic->ChangeTime.QuadPart = unix_ns_to_filetime(attributes->change_time_ns);
    if (attributes->object_type == INFS_OBJECT_DIRECTORY)
        basic->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    else if (attributes->object_type == INFS_OBJECT_SYMLINK)
        basic->FileAttributes = FILE_ATTRIBUTE_READONLY;
    else
        basic->FileAttributes = FILE_ATTRIBUTE_ARCHIVE;
}

static int bridge_alias_prefix_match(PCWSTR path, PCWSTR prefix)
{
    size_t prefix_length = wcslen(prefix);
    if (_wcsnicmp(path, prefix, prefix_length) != 0)
        return 0;
    return path[prefix_length] == L'\0' ||
           path[prefix_length] == L'\\' ||
           path[prefix_length] == L'/';
}

static int bridge_resolve_relative(PCWSTR relative,
                                   wchar_t out[INFS_PATH_MAX + 1u])
{
    if (!relative)
        relative = L"";
    if (wcslen(relative) > INFS_PATH_MAX)
        return 0;
    wcsncpy_s(out, INFS_PATH_MAX + 1u, relative, _TRUNCATE);

    for (unsigned pass = 0; pass < 32u; ++pass) {
        struct bridge_alias *best = NULL;
        size_t best_length = 0;
        for (struct bridge_alias *alias = g_bridge.aliases;
             alias; alias = alias->next) {
            size_t length = wcslen(alias->from);
            if (length > best_length &&
                bridge_alias_prefix_match(out, alias->from)) {
                best = alias;
                best_length = length;
            }
        }
        if (!best)
            break;

        wchar_t rewritten[INFS_PATH_MAX + 1u];
        PCWSTR suffix = out + best_length;
        int written = _snwprintf_s(
            rewritten, sizeof(rewritten) / sizeof(rewritten[0]), _TRUNCATE,
            L"%s%s", best->to, suffix);
        if (written < 0)
            return 0;
        if (_wcsicmp(rewritten, out) == 0)
            break;
        wcscpy_s(out, INFS_PATH_MAX + 1u, rewritten);
    }
    return 1;
}

static void bridge_add_alias(PCWSTR from, PCWSTR to)
{
    if (!from || !*from || !to || !*to ||
        wcslen(from) > INFS_PATH_MAX || wcslen(to) > INFS_PATH_MAX)
        return;
    struct bridge_alias *alias = calloc(1, sizeof(*alias));
    if (!alias)
        return;
    wcsncpy_s(alias->from, INFS_PATH_MAX + 1u, from, _TRUNCATE);
    wcsncpy_s(alias->to, INFS_PATH_MAX + 1u, to, _TRUNCATE);
    alias->next = g_bridge.aliases;
    g_bridge.aliases = alias;
}

static int wide_relative_to_infs(PCWSTR relative,
                                 char out[INFS_PATH_MAX + 1u])
{
    if (!relative)
        relative = L"";
    if (!*relative) {
        strcpy_s(out, INFS_PATH_MAX + 1u, "/");
        return 1;
    }

    wchar_t normalized[INFS_PATH_MAX + 1u];
    size_t length = wcslen(relative);
    if (length > INFS_PATH_MAX - 1u)
        return 0;
    normalized[0] = L'/';
    for (size_t i = 0; i < length; ++i) {
        wchar_t ch = relative[i] == L'\\' ? L'/' : relative[i];
        normalized[i + 1u] = ch;
    }
    normalized[length + 1u] = L'\0';

    int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                     normalized, -1, NULL, 0, NULL, NULL);
    if (needed <= 0 || needed > (int)INFS_PATH_MAX + 1)
        return 0;
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                               normalized, -1, out, INFS_PATH_MAX + 1u,
                               NULL, NULL) != 0;
}

static int make_child_infs_path(const char *parent, const char *name,
                                char out[INFS_PATH_MAX + 1u])
{
    int written;
    if (strcmp(parent, "/") == 0)
        written = snprintf(out, INFS_PATH_MAX + 1u, "/%s", name);
    else
        written = snprintf(out, INFS_PATH_MAX + 1u, "%s/%s", parent, name);
    return written > 0 && written <= (int)INFS_PATH_MAX;
}

static int relative_to_local_path(PCWSTR relative,
                                  wchar_t out[MAX_PATH * 4u])
{
    if (!relative || !*relative)
        return wcscpy_s(out, MAX_PATH * 4u, g_bridge.root) == 0;
    if (wcslen(relative) > INFS_PATH_MAX)
        return 0;
    return _snwprintf_s(out, MAX_PATH * 4u, _TRUNCATE,
                        L"%s\\%s", g_bridge.root, relative) >= 0;
}

static HRESULT status_to_hresult(infs_status status)
{
    switch (status) {
    case INFS_STATUS_OK: return S_OK;
    case INFS_STATUS_NOT_FOUND:
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    case INFS_STATUS_ALREADY_EXISTS:
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    case INFS_STATUS_NOT_DIRECTORY:
        return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    case INFS_STATUS_IS_DIRECTORY:
        return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    case INFS_STATUS_NOT_EMPTY:
        return HRESULT_FROM_WIN32(ERROR_DIR_NOT_EMPTY);
    case INFS_STATUS_NAME_TOO_LONG:
        return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    case INFS_STATUS_READ_ONLY:
        return HRESULT_FROM_WIN32(ERROR_WRITE_PROTECT);
    case INFS_STATUS_NO_SPACE:
        return HRESULT_FROM_WIN32(ERROR_DISK_FULL);
    case INFS_STATUS_NO_MEMORY:
        return E_OUTOFMEMORY;
    case INFS_STATUS_BUSY:
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    default:
        return HRESULT_FROM_WIN32(ERROR_IO_DEVICE);
    }
}

static int bridge_entry_compare(const void *left, const void *right)
{
    const struct bridge_dir_entry *a = left;
    const struct bridge_dir_entry *b = right;
    int result = g_projfs.name_compare(a->name, b->name);
    return result < 0 ? -1 : result > 0 ? 1 : 0;
}

static void enum_free_entries(struct bridge_enum *session)
{
    free(session->entries);
    session->entries = NULL;
    session->count = 0;
    session->index = 0;
    session->search[0] = L'\0';
}

static struct bridge_enum *enum_find(const GUID *id)
{
    for (struct bridge_enum *session = g_bridge.enums;
         session; session = session->next) {
        if (memcmp(&session->id, id, sizeof(*id)) == 0)
            return session;
    }
    return NULL;
}

static void enum_remove(const GUID *id)
{
    struct bridge_enum **link = &g_bridge.enums;
    while (*link) {
        if (memcmp(&(*link)->id, id, sizeof(*id)) == 0) {
            struct bridge_enum *victim = *link;
            *link = victim->next;
            enum_free_entries(victim);
            free(victim);
            return;
        }
        link = &(*link)->next;
    }
}

static HRESULT enum_load(struct bridge_enum *session, PCWSTR search_expression)
{
    char path[INFS_PATH_MAX + 1u];
    if (!wide_relative_to_infs(session->relative_path, path))
        return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);

    struct infs_dir_item *items = NULL;
    size_t count = 0;
    EnterCriticalSection(&g_bridge.lock);
    infs_status status = infs_list_dir(g_bridge.volume, path, &items, &count);
    LeaveCriticalSection(&g_bridge.lock);
    if (status != INFS_STATUS_OK)
        return status_to_hresult(status);

    struct bridge_dir_entry *entries = calloc(count ? count : 1u,
                                               sizeof(*entries));
    if (!entries) {
        infs_free_dir_items(items);
        return E_OUTOFMEMORY;
    }

    size_t accepted = 0;
    for (size_t i = 0; i < count; ++i) {
        wchar_t wide_name[INFS_NAME_MAX + 1u];
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                 items[i].name, -1, wide_name,
                                 (int)(sizeof(wide_name) /
                                       sizeof(wide_name[0]))))
            continue;
        if (search_expression && *search_expression &&
            !g_projfs.name_match(wide_name, search_expression))
            continue;

        char child[INFS_PATH_MAX + 1u];
        if (!make_child_infs_path(path, items[i].name, child))
            continue;
        struct infs_attributes attributes;
        EnterCriticalSection(&g_bridge.lock);
        status = infs_get_attributes(g_bridge.volume, child, &attributes);
        LeaveCriticalSection(&g_bridge.lock);
        if (status != INFS_STATUS_OK)
            continue;

        wcscpy_s(entries[accepted].name,
                 sizeof(entries[accepted].name) /
                     sizeof(entries[accepted].name[0]),
                 wide_name);
        entries[accepted].attributes = attributes;
        ++accepted;
    }
    infs_free_dir_items(items);

    qsort(entries, accepted, sizeof(*entries), bridge_entry_compare);
    enum_free_entries(session);
    session->entries = entries;
    session->count = accepted;
    session->index = 0;
    if (search_expression)
        wcsncpy_s(session->search,
                  sizeof(session->search) / sizeof(session->search[0]),
                  search_expression, _TRUNCATE);
    return S_OK;
}

static HRESULT CALLBACK bridge_start_enum(
    const PRJ_CALLBACK_DATA *callback_data, const GUID *enumeration_id)
{
    struct bridge_enum *session = calloc(1, sizeof(*session));
    if (!session)
        return E_OUTOFMEMORY;
    session->id = *enumeration_id;

    EnterCriticalSection(&g_bridge.lock);
    if (!bridge_identity_relative(callback_data, session->relative_path)) {
        LeaveCriticalSection(&g_bridge.lock);
        free(session);
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    session->next = g_bridge.enums;
    g_bridge.enums = session;
    LeaveCriticalSection(&g_bridge.lock);
    return S_OK;
}

static HRESULT CALLBACK bridge_get_enum(
    const PRJ_CALLBACK_DATA *callback_data, const GUID *enumeration_id,
    PCWSTR search_expression, PRJ_DIR_ENTRY_BUFFER_HANDLE buffer)
{
    EnterCriticalSection(&g_bridge.lock);
    struct bridge_enum *session = enum_find(enumeration_id);
    LeaveCriticalSection(&g_bridge.lock);
    if (!session)
        return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);

    int restart =
        (callback_data->Flags & PRJ_CB_DATA_FLAG_ENUM_RESTART_SCAN) != 0;
    int search_changed =
        (!search_expression && session->search[0]) ||
        (search_expression &&
         _wcsicmp(search_expression, session->search) != 0);
    if (!session->entries || restart || search_changed) {
        HRESULT hr = enum_load(session, search_expression);
        if (FAILED(hr))
            return hr;
    }

    while (session->index < session->count) {
        PRJ_FILE_BASIC_INFO basic;
        attributes_to_basic(&session->entries[session->index].attributes,
                            &basic);
        HRESULT hr = g_projfs.fill_dir(
            session->entries[session->index].name, &basic, buffer);
        if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER))
            return S_OK;
        if (FAILED(hr))
            return hr;
        ++session->index;
    }
    return S_OK;
}

static HRESULT CALLBACK bridge_end_enum(
    const PRJ_CALLBACK_DATA *callback_data, const GUID *enumeration_id)
{
    (void)callback_data;
    EnterCriticalSection(&g_bridge.lock);
    enum_remove(enumeration_id);
    LeaveCriticalSection(&g_bridge.lock);
    return S_OK;
}

static HRESULT CALLBACK bridge_get_placeholder(
    const PRJ_CALLBACK_DATA *callback_data)
{
    wchar_t current_relative[INFS_PATH_MAX + 1u];
    EnterCriticalSection(&g_bridge.lock);
    int have_relative =
        bridge_identity_relative(callback_data, current_relative);
    LeaveCriticalSection(&g_bridge.lock);
    if (!have_relative)
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    char path[INFS_PATH_MAX + 1u];
    if (!wide_relative_to_infs(current_relative, path))
        return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);

    struct infs_attributes attributes;
    EnterCriticalSection(&g_bridge.lock);
    infs_status status =
        infs_get_attributes(g_bridge.volume, path, &attributes);
    LeaveCriticalSection(&g_bridge.lock);
    if (status != INFS_STATUS_OK)
        return status_to_hresult(status);

    PRJ_PLACEHOLDER_INFO placeholder;
    memset(&placeholder, 0, sizeof(placeholder));
    attributes_to_basic(&attributes, &placeholder.FileBasicInfo);
    EnterCriticalSection(&g_bridge.lock);
    struct bridge_identity *identity =
        bridge_remember_identity(attributes.object_id,
                                 current_relative);
    if (identity)
        bridge_fill_version(&attributes, identity->token,
                            &placeholder.VersionInfo);
    LeaveCriticalSection(&g_bridge.lock);
    if (!identity)
        return E_OUTOFMEMORY;
    return g_projfs.write_placeholder(
        callback_data->NamespaceVirtualizationContext,
        callback_data->FilePathName, &placeholder, sizeof(placeholder));
}

static int64_t bridge_read_symlink(const char *path, void *buffer,
                                   size_t size, uint64_t offset)
{
    char target[INFS_PATH_MAX + 1u];
    size_t length = 0;
    infs_status status =
        infs_read_symlink(g_bridge.volume, path, target,
                          sizeof(target), &length);
    if (status != INFS_STATUS_OK)
        return status;
    if (offset >= length)
        return 0;
    size_t available = length - (size_t)offset;
    if (size > available)
        size = available;
    memcpy(buffer, target + offset, size);
    return (int64_t)size;
}

static HRESULT CALLBACK bridge_get_file_data(
    const PRJ_CALLBACK_DATA *callback_data, UINT64 byte_offset, UINT32 length)
{
    wchar_t current_relative[INFS_PATH_MAX + 1u];
    EnterCriticalSection(&g_bridge.lock);
    int have_relative =
        bridge_identity_relative(callback_data, current_relative);
    if (have_relative &&
        !bridge_version_is_ours(callback_data->VersionInfo)) {
        wchar_t resolved[INFS_PATH_MAX + 1u];
        if (bridge_resolve_relative(current_relative, resolved))
            wcscpy_s(current_relative, INFS_PATH_MAX + 1u, resolved);
    }
    LeaveCriticalSection(&g_bridge.lock);
    if (!have_relative)
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    char path[INFS_PATH_MAX + 1u];
    if (!wide_relative_to_infs(current_relative, path))
        return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);

    struct infs_attributes attributes;
    EnterCriticalSection(&g_bridge.lock);
    infs_status status =
        infs_get_attributes(g_bridge.volume, path, &attributes);
    LeaveCriticalSection(&g_bridge.lock);
    if (status != INFS_STATUS_OK)
        return status_to_hresult(status);
    if (attributes.object_type == INFS_OBJECT_DIRECTORY)
        return HRESULT_FROM_WIN32(ERROR_DIRECTORY);

    void *buffer = g_projfs.alloc(
        callback_data->NamespaceVirtualizationContext, length);
    if (!buffer)
        return E_OUTOFMEMORY;

    EnterCriticalSection(&g_bridge.lock);
    int64_t got = attributes.object_type == INFS_OBJECT_SYMLINK ?
        bridge_read_symlink(path, buffer, length, byte_offset) :
        infs_read_file(g_bridge.volume, path, buffer, length, byte_offset);
    LeaveCriticalSection(&g_bridge.lock);
    if (got < 0) {
        g_projfs.free_buffer(buffer);
        return status_to_hresult((infs_status)got);
    }

    HRESULT hr = g_projfs.write_data(
        callback_data->NamespaceVirtualizationContext,
        &callback_data->DataStreamId, buffer, byte_offset, (UINT32)got);
    g_projfs.free_buffer(buffer);
    return hr;
}

static infs_status bridge_create_empty(const char *path, int is_directory)
{
    struct infs_create_options options;
    memset(&options, 0, sizeof(options));
    options.posix_permissions = is_directory ? 0755u : 0644u;
    infs_status status = is_directory ?
        infs_mkdir(g_bridge.volume, path, &options) :
        infs_create_file(g_bridge.volume, path, &options);
    if (status == INFS_STATUS_ALREADY_EXISTS)
        return INFS_STATUS_OK;
    return status;
}

static infs_status bridge_sync_local_file(PCWSTR relative)
{
    char path[INFS_PATH_MAX + 1u];
    wchar_t local[MAX_PATH * 4u];
    if (!wide_relative_to_infs(relative, path) ||
        !relative_to_local_path(relative, local))
        return INFS_STATUS_NAME_TOO_LONG;

    HANDLE input = CreateFileW(local, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE |
                                   FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                               NULL);
    if (input == INVALID_HANDLE_VALUE)
        return INFS_STATUS_IO_ERROR;

    LARGE_INTEGER local_length;
    if (!GetFileSizeEx(input, &local_length) || local_length.QuadPart < 0) {
        CloseHandle(input);
        return INFS_STATUS_IO_ERROR;
    }

    struct infs_attributes existing;
    memset(&existing, 0, sizeof(existing));
    infs_status status =
        infs_get_attributes(g_bridge.volume, path, &existing);
    uint64_t old_size = 0;
    if (status == INFS_STATUS_NOT_FOUND) {
        status = bridge_create_empty(path, 0);
    } else if (status == INFS_STATUS_OK) {
        if (existing.object_type == INFS_OBJECT_DIRECTORY) {
            CloseHandle(input);
            return INFS_STATUS_INVALID_ARGUMENT;
        }
        old_size = existing.logical_size;
    }
    if (status != INFS_STATUS_OK) {
        CloseHandle(input);
        return status;
    }

    uint8_t *local_buffer = malloc(BRIDGE_READ_CHUNK);
    uint8_t *old_buffer = malloc(BRIDGE_READ_CHUNK);
    if (!local_buffer || !old_buffer) {
        free(local_buffer);
        free(old_buffer);
        CloseHandle(input);
        return INFS_STATUS_NO_MEMORY;
    }

    uint64_t offset = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(input, local_buffer, BRIDGE_READ_CHUNK, &got, NULL)) {
            status = INFS_STATUS_IO_ERROR;
            break;
        }
        if (!got)
            break;

        size_t old_available = 0;
        if (offset < old_size) {
            uint64_t remaining = old_size - offset;
            size_t wanted = got;
            if (remaining < wanted)
                wanted = (size_t)remaining;
            int64_t read = infs_read_file(
                g_bridge.volume, path, old_buffer, wanted, offset);
            if (read < 0 || (size_t)read != wanted) {
                status = read < 0 ? (infs_status)read :
                                    INFS_STATUS_IO_ERROR;
                break;
            }
            old_available = wanted;
        }

        g_bridge.stats.bytes_examined += got;

        /*
         * ProjFS exposes completed local file contents rather than native
         * byte-range write callbacks. Compare at filesystem-block granularity
         * and write back only changed runs. A 4 KiB edit of a multi-gigabyte
         * file therefore becomes a small CoW mutation instead of a whole-file
         * rewrite.
         */
        size_t cursor = 0;
        while (cursor < got) {
            size_t span = INFS_BLOCK_SIZE;
            if (span > (size_t)got - cursor)
                span = (size_t)got - cursor;

            int changed =
                cursor + span > old_available ||
                memcmp(local_buffer + cursor, old_buffer + cursor, span) != 0;
            if (!changed) {
                cursor += span;
                continue;
            }

            size_t run_start = cursor;
            cursor += span;
            while (cursor < got) {
                span = INFS_BLOCK_SIZE;
                if (span > (size_t)got - cursor)
                    span = (size_t)got - cursor;
                changed =
                    cursor + span > old_available ||
                    memcmp(local_buffer + cursor,
                           old_buffer + cursor, span) != 0;
                if (!changed)
                    break;
                cursor += span;
            }

            size_t run_length = cursor - run_start;
            int64_t written = infs_write_file_buffered(
                g_bridge.volume, path, local_buffer + run_start,
                run_length, offset + run_start);
            if (written != (int64_t)run_length) {
                status = written < 0 ? (infs_status)written :
                                       INFS_STATUS_IO_ERROR;
                break;
            }
            g_bridge.stats.bytes_written += run_length;
        }
        if (status != INFS_STATUS_OK)
            break;
        offset += got;
    }
    free(local_buffer);
    free(old_buffer);

    if (status == INFS_STATUS_OK && old_size > offset)
        status = infs_truncate_file(g_bridge.volume, path, offset);

    FILETIME creation_time, access_time, write_time;
    if (status == INFS_STATUS_OK &&
        GetFileTime(input, &creation_time, &access_time, &write_time)) {
        ULARGE_INTEGER ft;
        ft.LowPart = write_time.dwLowDateTime;
        ft.HighPart = write_time.dwHighDateTime;
        const uint64_t epoch = UINT64_C(116444736000000000);
        int64_t mtime_ns = 0;
        if (ft.QuadPart >= epoch)
            mtime_ns = (int64_t)((ft.QuadPart - epoch) * UINT64_C(100));
        struct infs_time_update update;
        memset(&update, 0, sizeof(update));
        update.access_action = INFS_TIME_OMIT;
        update.modification_action = INFS_TIME_SET;
        update.modification_time_ns = mtime_ns;
        status = infs_set_times(g_bridge.volume, path, &update);
    }
    CloseHandle(input);

    if (status == INFS_STATUS_OK) {
        g_bridge.stats.files_imported++;
        status = bridge_note_mutation_locked();
    }
    return status;
}

static infs_status bridge_delete_path(const char *path, int is_directory)
{
    infs_status status = is_directory ?
        infs_rmdir(g_bridge.volume, path) :
        infs_unlink(g_bridge.volume, path);
    return status == INFS_STATUS_NOT_FOUND ? INFS_STATUS_OK : status;
}

static infs_status bridge_import_local_tree(PCWSTR relative)
{
    wchar_t local[MAX_PATH * 4u];
    char path[INFS_PATH_MAX + 1u];
    if (!relative_to_local_path(relative, local) ||
        !wide_relative_to_infs(relative, path))
        return INFS_STATUS_NAME_TOO_LONG;

    DWORD attributes = GetFileAttributesW(local);
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return INFS_STATUS_NOT_FOUND;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return bridge_sync_local_file(relative);

    infs_status status = bridge_create_empty(path, 1);
    if (status != INFS_STATUS_OK)
        return status;
    status = bridge_note_mutation_locked();
    if (status != INFS_STATUS_OK)
        return status;

    wchar_t pattern[MAX_PATH * 4u];
    if (_snwprintf_s(pattern, sizeof(pattern) / sizeof(pattern[0]),
                     _TRUNCATE, L"%s\\*", local) < 0)
        return INFS_STATUS_NAME_TOO_LONG;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND ?
               INFS_STATUS_OK : INFS_STATUS_IO_ERROR;

    do {
        if (wcscmp(data.cFileName, L".") == 0 ||
            wcscmp(data.cFileName, L"..") == 0)
            continue;
        wchar_t child[INFS_PATH_MAX + 1u];
        if (!relative || !*relative) {
            if (_snwprintf_s(child,
                             sizeof(child) / sizeof(child[0]), _TRUNCATE,
                             L"%s", data.cFileName) < 0) {
                status = INFS_STATUS_NAME_TOO_LONG;
                break;
            }
        } else if (_snwprintf_s(child,
                                sizeof(child) / sizeof(child[0]), _TRUNCATE,
                                L"%s\\%s", relative,
                                data.cFileName) < 0) {
            status = INFS_STATUS_NAME_TOO_LONG;
            break;
        }
        status = bridge_import_local_tree(child);
        if (status != INFS_STATUS_OK)
            break;
    } while (FindNextFileW(find, &data));
    DWORD error = GetLastError();
    FindClose(find);
    if (status == INFS_STATUS_OK && error != ERROR_NO_MORE_FILES)
        status = INFS_STATUS_IO_ERROR;
    return status;
}

static int bridge_trace_enabled(void)
{
    wchar_t value[4] = {0};
    return GetEnvironmentVariableW(
               L"INFILTRATORFS_BRIDGE_TRACE", value,
               (DWORD)(sizeof(value) / sizeof(value[0]))) != 0;
}

static HRESULT CALLBACK bridge_notification(
    const PRJ_CALLBACK_DATA *callback_data, BOOLEAN is_directory,
    PRJ_NOTIFICATION notification, PCWSTR destination_file_name,
    PRJ_NOTIFICATION_PARAMETERS *operation_parameters)
{
    (void)operation_parameters;
    if (bridge_trace_enabled()) {
        fwprintf(stderr,
                 L"[ProjFS] notification=0x%08lx dir=%d path='%ls' destination='%ls'\n",
                 (unsigned long)notification, is_directory ? 1 : 0,
                 callback_data && callback_data->FilePathName ?
                     callback_data->FilePathName : L"",
                 destination_file_name ? destination_file_name : L"");
        fflush(stderr);
    }
    wchar_t current_relative[INFS_PATH_MAX + 1u];
    EnterCriticalSection(&g_bridge.lock);
    int have_current =
        bridge_identity_relative(callback_data, current_relative);
    LeaveCriticalSection(&g_bridge.lock);
    if (!have_current && callback_data->FilePathName &&
        *callback_data->FilePathName)
        return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);

    char source[INFS_PATH_MAX + 1u] = "/";
    if (have_current &&
        !wide_relative_to_infs(current_relative, source))
        return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);

    infs_status status = INFS_STATUS_OK;
    EnterCriticalSection(&g_bridge.lock);
    switch (notification) {
    case PRJ_NOTIFICATION_NEW_FILE_CREATED:
        status = bridge_create_empty(source, is_directory ? 1 : 0);
        if (status == INFS_STATUS_OK)
            status = bridge_note_mutation_locked();
        operation_parameters->PostCreate.NotificationMask =
            BRIDGE_NOTIFY_PERSIST_MASK;
        break;

    case PRJ_NOTIFICATION_FILE_OVERWRITTEN:
        /*
         * Do not import here: CREATE_ALWAYS/overwrite notifications arrive
         * before the writer necessarily closes its handle. Keep receiving the
         * close notification and commit the completed file there.
         */
        operation_parameters->PostCreate.NotificationMask =
            BRIDGE_NOTIFY_PERSIST_MASK;
        break;

    case PRJ_NOTIFICATION_FILE_PRE_CONVERT_TO_FULL:
        /*
         * A projected file is about to become locally writable. The close
         * notification below is the durability boundary back to
         * InfiltratorFS.
         */
        break;

    case PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_FILE_MODIFIED:
        if (!is_directory)
            status = bridge_sync_local_file(
                have_current ? current_relative :
                               callback_data->FilePathName);
        break;

    case PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_FILE_DELETED:
        status = bridge_delete_path(source, is_directory ? 1 : 0);
        if (status == INFS_STATUS_OK) {
            if (have_current)
                bridge_forget_identity_prefix(current_relative);
            status = bridge_note_mutation_locked();
        }
        break;

    case PRJ_NOTIFICATION_FILE_RENAMED:
        if ((!callback_data->FilePathName ||
             !*callback_data->FilePathName) &&
            destination_file_name && *destination_file_name) {
            status = bridge_import_local_tree(destination_file_name);
        } else if ((!destination_file_name ||
                    !*destination_file_name) &&
                   callback_data->FilePathName &&
                   *callback_data->FilePathName) {
            status = bridge_delete_path(source, is_directory ? 1 : 0);
            if (status == INFS_STATUS_OK) {
                if (have_current)
                    bridge_forget_identity_prefix(current_relative);
                status = bridge_note_mutation_locked();
            }
        } else if (destination_file_name && *destination_file_name) {
            char destination[INFS_PATH_MAX + 1u];
            if (!wide_relative_to_infs(destination_file_name, destination)) {
                status = INFS_STATUS_NAME_TOO_LONG;
                break;
            }
            status = infs_rename(g_bridge.volume, source, destination);
            if (status == INFS_STATUS_OK) {
                PCWSTR old_relative =
                    have_current ? current_relative :
                                   callback_data->FilePathName;
                bridge_add_alias(old_relative, destination_file_name);
                bridge_update_identity_prefix(old_relative,
                                              destination_file_name);
                operation_parameters->FileRenamed.NotificationMask =
                    BRIDGE_NOTIFY_PERSIST_MASK;
                status = bridge_note_mutation_locked();
            }
        }
        break;

    case PRJ_NOTIFICATION_HARDLINK_CREATED:
        if (!is_directory && destination_file_name &&
            *destination_file_name) {
            if (!callback_data->FilePathName ||
                !*callback_data->FilePathName) {
                status = bridge_sync_local_file(destination_file_name);
            } else {
                char destination[INFS_PATH_MAX + 1u];
                if (!wide_relative_to_infs(destination_file_name,
                                           destination)) {
                    status = INFS_STATUS_NAME_TOO_LONG;
                    break;
                }
                status = infs_link_file(g_bridge.volume, source, destination);
                if (status == INFS_STATUS_OK)
                    status = bridge_note_mutation_locked();
            }
        }
        break;

    default:
        break;
    }
    LeaveCriticalSection(&g_bridge.lock);
    if (bridge_trace_enabled()) {
        fwprintf(stderr, L"[ProjFS] notification result=%d\n", (int)status);
        fflush(stderr);
    }
    return status_to_hresult(status);
}

static void bridge_free_enums(void)
{
    while (g_bridge.enums) {
        struct bridge_enum *next = g_bridge.enums->next;
        enum_free_entries(g_bridge.enums);
        free(g_bridge.enums);
        g_bridge.enums = next;
    }
    while (g_bridge.aliases) {
        struct bridge_alias *next = g_bridge.aliases->next;
        free(g_bridge.aliases);
        g_bridge.aliases = next;
    }
    while (g_bridge.identities) {
        struct bridge_identity *next = g_bridge.identities->next;
        free(g_bridge.identities);
        g_bridge.identities = next;
    }
}

static int remove_tree(const wchar_t *root)
{
    wchar_t pattern[MAX_PATH * 4u];
    if (_snwprintf_s(pattern, sizeof(pattern) / sizeof(pattern[0]),
                     _TRUNCATE, L"%s\\*", root) < 0)
        return 0;

    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(data.cFileName, L".") == 0 ||
                wcscmp(data.cFileName, L"..") == 0)
                continue;
            wchar_t child[MAX_PATH * 4u];
            if (_snwprintf_s(child, sizeof(child) / sizeof(child[0]),
                             _TRUNCATE, L"%s\\%s", root,
                             data.cFileName) < 0)
                continue;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_tree(child);
            else {
                SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child);
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    SetFileAttributesW(root, FILE_ATTRIBUTE_NORMAL);
    return RemoveDirectoryW(root) != 0 ||
           GetLastError() == ERROR_FILE_NOT_FOUND;
}

static int choose_drive_letter(wchar_t out[3])
{
    DWORD mask = GetLogicalDrives();
    const wchar_t *order = L"IJKLMNOPQRSTUVWXYZDEFGH";
    for (const wchar_t *p = order; *p; ++p) {
        unsigned bit = (unsigned)(*p - L'A');
        if ((mask & (1u << bit)) == 0u) {
            out[0] = *p;
            out[1] = L':';
            out[2] = L'\0';
            return 1;
        }
    }
    return 0;
}

static int create_bridge_root(wchar_t out[MAX_PATH * 4u])
{
    wchar_t base[MAX_PATH * 4u];
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
                         NULL, SHGFP_TYPE_CURRENT, base) != S_OK)
        return 0;

    wchar_t parent[MAX_PATH * 4u];
    if (_snwprintf_s(parent, sizeof(parent) / sizeof(parent[0]),
                     _TRUNCATE, L"%s\\InfiltratorFS", base) < 0)
        return 0;
    CreateDirectoryW(parent, NULL);
    if (GetLastError() != ERROR_SUCCESS &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return 0;

    if (_snwprintf_s(parent, sizeof(parent) / sizeof(parent[0]),
                     _TRUNCATE, L"%s\\InfiltratorFS\\Bridge", base) < 0)
        return 0;
    CreateDirectoryW(parent, NULL);
    if (GetLastError() != ERROR_SUCCESS &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return 0;

    GUID guid;
    if (FAILED(CoCreateGuid(&guid)))
        return 0;
    wchar_t guid_text[64];
    if (!StringFromGUID2(&guid, guid_text,
                         (int)(sizeof(guid_text) /
                               sizeof(guid_text[0]))))
        return 0;
    for (wchar_t *p = guid_text; *p; ++p) {
        if (*p == L'{' || *p == L'}')
            *p = L'_';
    }

    if (_snwprintf_s(out, MAX_PATH * 4u, _TRUNCATE,
                     L"%s\\%s", parent, guid_text) < 0)
        return 0;
    return CreateDirectoryW(out, NULL) != 0;
}

static void show_bridge_error(HWND owner, const wchar_t *action, HRESULT hr)
{
    wchar_t message[768];
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) ||
        hr == HRESULT_FROM_WIN32(ERROR_INVALID_FUNCTION)) {
        _snwprintf_s(message, sizeof(message) / sizeof(message[0]),
                     _TRUNCATE,
                     L"%s failed.\n\nWindows Projected File System (ProjFS) "
                     L"is not enabled. Enable the optional Windows feature "
                     L"'Windows Projected File System', then try again.\n\n"
                     L"HRESULT: 0x%08lx",
                     action, (unsigned long)hr);
    } else {
        _snwprintf_s(message, sizeof(message) / sizeof(message[0]),
                     _TRUNCATE, L"%s failed (HRESULT 0x%08lx).",
                     action, (unsigned long)hr);
    }
    MessageBoxW(owner, message, L"InfiltratorFS Windows Bridge",
                MB_OK | MB_ICONERROR);
}

int infs_windows_bridge_start(struct infs_volume *volume, HWND owner,
                              wchar_t *drive_out, size_t drive_out_count)
{
    if (!volume || !volume->writable || g_bridge.active)
        return 0;
    if (!bridge_require_projfs(owner))
        return 0;

    memset(&g_bridge, 0, sizeof(g_bridge));
    g_bridge.volume = volume;
    g_bridge.owner = owner;
    InitializeCriticalSection(&g_bridge.lock);
    g_bridge.lock_ready = 1;

    if (!choose_drive_letter(g_bridge.drive) ||
        !create_bridge_root(g_bridge.root)) {
        infs_windows_bridge_stop();
        MessageBoxW(owner,
                    L"Could not allocate a Windows bridge drive/root.",
                    L"InfiltratorFS Windows Bridge",
                    MB_OK | MB_ICONERROR);
        return 0;
    }

    GUID instance_id;
    if (FAILED(CoCreateGuid(&instance_id))) {
        infs_windows_bridge_stop();
        return 0;
    }

    HRESULT hr = g_projfs.mark_directory(
        g_bridge.root, NULL, NULL, &instance_id);
    if (FAILED(hr)) {
        show_bridge_error(owner, L"Prepare virtualization root", hr);
        infs_windows_bridge_stop();
        return 0;
    }

    PRJ_CALLBACKS callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.StartDirectoryEnumerationCallback = bridge_start_enum;
    callbacks.EndDirectoryEnumerationCallback = bridge_end_enum;
    callbacks.GetDirectoryEnumerationCallback = bridge_get_enum;
    callbacks.GetPlaceholderInfoCallback = bridge_get_placeholder;
    callbacks.GetFileDataCallback = bridge_get_file_data;
    callbacks.NotificationCallback = bridge_notification;

    PRJ_NOTIFICATION_MAPPING notification;
    memset(&notification, 0, sizeof(notification));
    notification.NotificationRoot = L"";
    notification.NotificationBitMask =
        BRIDGE_NOTIFY_ROOT_MASK;

    PRJ_STARTVIRTUALIZING_OPTIONS options;
    memset(&options, 0, sizeof(options));
    options.PoolThreadCount = BRIDGE_PROVIDER_THREADS;
    options.ConcurrentThreadCount = BRIDGE_PROVIDER_THREADS;
    options.NotificationMappings = &notification;
    options.NotificationMappingsCount = 1;

    hr = g_projfs.start(g_bridge.root, &callbacks, &g_bridge,
                              &options, &g_bridge.context);
    if (FAILED(hr)) {
        show_bridge_error(owner, L"Start Projected File System provider", hr);
        infs_windows_bridge_stop();
        return 0;
    }

    /*
     * The manager runs elevated so it can open raw partitions. A DOS-device
     * alias created by that elevated process can be invisible to the normal
     * unelevated Explorer process because Windows keeps per-logon/UAC DOS
     * device namespaces. Treat the drive letter as an optional convenience,
     * never as the projection itself. The ProjFS virtualization root is the
     * authoritative Explorer endpoint and is accessible in either token.
     */
    int have_drive_alias = 0;
    if (_snwprintf_s(g_bridge.dos_target,
                     sizeof(g_bridge.dos_target) /
                         sizeof(g_bridge.dos_target[0]),
                     _TRUNCATE, L"\\??\\%s", g_bridge.root) >= 0 &&
        DefineDosDeviceW(DDD_RAW_TARGET_PATH, g_bridge.drive,
                         g_bridge.dos_target)) {
        have_drive_alias = 1;
    } else {
        g_bridge.drive[0] = L'\0';
        g_bridge.dos_target[0] = L'\0';
    }

    g_bridge.active = 1;
    if (!CreateTimerQueueTimer(
            &g_bridge.flush_timer, NULL, bridge_idle_flush, NULL,
            BRIDGE_IDLE_FLUSH_MS, BRIDGE_IDLE_FLUSH_MS,
            WT_EXECUTEDEFAULT)) {
        g_bridge.flush_timer = NULL;
    }
    if (drive_out && drive_out_count) {
        if (have_drive_alias)
            wcsncpy_s(drive_out, drive_out_count, g_bridge.drive, _TRUNCATE);
        else
            drive_out[0] = L'\0';
    }

    wchar_t no_explorer[8] = {0};
    DWORD no_explorer_length = GetEnvironmentVariableW(
        L"INFILTRATORFS_BRIDGE_NO_EXPLORER", no_explorer,
        (DWORD)(sizeof(no_explorer) / sizeof(no_explorer[0])));
    if (!no_explorer_length)
        ShellExecuteW(owner, L"open", g_bridge.root,
                      NULL, NULL, SW_SHOWNORMAL);
    return 1;
}

void infs_windows_bridge_stop(void)
{
    if (g_bridge.drive[0] && g_bridge.dos_target[0]) {
        DefineDosDeviceW(DDD_REMOVE_DEFINITION |
                         DDD_EXACT_MATCH_ON_REMOVE |
                         DDD_RAW_TARGET_PATH,
                         g_bridge.drive, g_bridge.dos_target);
    }

    if (g_bridge.context) {
        g_projfs.stop(g_bridge.context);
        g_bridge.context = NULL;
    }

    if (g_bridge.flush_timer) {
        DeleteTimerQueueTimer(NULL, g_bridge.flush_timer,
                              INVALID_HANDLE_VALUE);
        g_bridge.flush_timer = NULL;
    }

    if (g_bridge.lock_ready) {
        EnterCriticalSection(&g_bridge.lock);
        (void)bridge_publish_locked();
        bridge_free_enums();
        LeaveCriticalSection(&g_bridge.lock);
        DeleteCriticalSection(&g_bridge.lock);
    }

    if (g_bridge.root[0])
        remove_tree(g_bridge.root);

    memset(&g_bridge, 0, sizeof(g_bridge));
}

int infs_windows_bridge_get_stats(
    struct infs_windows_bridge_stats *stats)
{
    if (!stats || !g_bridge.lock_ready)
        return 0;
    EnterCriticalSection(&g_bridge.lock);
    *stats = g_bridge.stats;
    LeaveCriticalSection(&g_bridge.lock);
    return 1;
}

int infs_windows_bridge_active(void)
{
    return g_bridge.active;
}

int infs_windows_bridge_root(wchar_t *root_out, size_t root_out_count)
{
    if (!g_bridge.active || !root_out || !root_out_count)
        return 0;
    wcsncpy_s(root_out, root_out_count, g_bridge.root, _TRUNCATE);
    return root_out[0] != L'\0';
}
#endif
