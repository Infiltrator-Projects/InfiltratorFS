// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Linux storage target discovery for the native InfiltratorFS Manager.
 *
 * Safety rule: targets derived from lsblk are not presented as format-capable
 * until canonical paths for /, /boot and /boot/efi plus their block ancestors
 * have been excluded. Mount-state refresh is observational only; privileged
 * mutation remains in the separately validated Manager helper.
 */
#include "infiltratorfs-manager-storage.h"
#include "infiltratorfs-manager-contract.h"

#include "infiltratr/core.h"
#include "infiltratr/posix.h"
#include "infiltratr/posix_path.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void target_free(gpointer data)
{
    Target *target = data;
    if (!target)
        return;
    g_free(target->path);
    g_free(target->name);
    g_free(target->media);
    g_free(target->filesystem);
    g_free(target->label);
    g_free(target->mountpoint);
    g_free(target->mount_fstype);
    g_free(target);
}

Target *target_new(const char *path, gboolean block)
{
    Target *target = g_new0(Target, 1);
    if (!target)
        return NULL;
    target->path = g_strdup(path ? path : "");
    target->block = block;
    target->name = g_strdup(infiltratr_path_basename(path ? path : ""));
    target->media = g_strdup(
        block ? "Fixed disk" : infilfs_manager_copy()->image_file_type);
    target->filesystem = g_strdup("");
    target->label = g_strdup("");
    target->mountpoint = g_strdup("");
    target->mount_fstype = g_strdup("");
    if (!target->path || !target->name || !target->media ||
        !target->filesystem || !target->label || !target->mountpoint ||
        !target->mount_fstype) {
        target_free(target);
        return NULL;
    }
    return target;
}

void replace_string(char **slot, const char *value)
{
    char *copy = g_strdup(value ? value : "");
    if (!copy)
        return;
    g_free(*slot);
    *slot = copy;
}

gboolean spawn_capture(const char *const argv[], char **output,
                              char **error_output, int *wait_status)
{
    GError *error = NULL;
    gchar *stdout_text = NULL;
    gchar *stderr_text = NULL;
    gint status = 0;
    gboolean spawned = g_spawn_sync(NULL, (gchar **)argv, NULL,
                                    G_SPAWN_SEARCH_PATH, NULL, NULL,
                                    &stdout_text, &stderr_text, &status, &error);
    if (!spawned) {
        if (error_output)
            *error_output = g_strdup(error ? error->message : "command failed");
        g_clear_error(&error);
        g_free(stdout_text);
        g_free(stderr_text);
        if (wait_status)
            *wait_status = -1;
        return FALSE;
    }
    if (output)
        *output = stdout_text;
    else
        g_free(stdout_text);
    if (error_output)
        *error_output = stderr_text;
    else
        g_free(stderr_text);
    if (wait_status)
        *wait_status = status;
    return TRUE;
}

gboolean command_success(const char *const argv[], char **output)
{
    char *error_output = NULL;
    int status = 0;
    gboolean spawned = spawn_capture(argv, output, &error_output, &status);
    g_free(error_output);
    if (!spawned)
        return FALSE;
    GError *error = NULL;
    gboolean okay = g_spawn_check_wait_status(status, &error);
    g_clear_error(&error);
    if (!okay && output && *output) {
        g_free(*output);
        *output = NULL;
    }
    return okay;
}

char *infiltratorfs_manager_canonical_path(const char *path)
{
    if (!path || !*path)
        return g_strdup("");
    char resolved[PATH_MAX];
    if (infiltratr_realpath_copy(path, resolved, sizeof(resolved)))
        return g_strdup(resolved);
    return g_strdup(path);
}

static GHashTable *protected_devices(void)
{
    GHashTable *set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    static const char *mounts[] = { "/", "/boot", "/boot/efi", NULL };
    for (size_t i = 0; mounts[i]; ++i) {
        const char *findmnt[] = { "findmnt", "-nro", "SOURCE", mounts[i], NULL };
        char *source = NULL;
        if (!command_success(findmnt, &source) || !source)
            continue;
        g_strstrip(source);
        if (!g_str_has_prefix(source, "/dev/")) {
            g_free(source);
            continue;
        }
        char *real = infiltratorfs_manager_canonical_path(source);
        if (real && *real)
            g_hash_table_add(set, real);
        else
            g_free(real);

        const char *lsblk[] = { "lsblk", "-srnpo", "NAME", source, NULL };
        char *ancestors = NULL;
        if (command_success(lsblk, &ancestors) && ancestors) {
            gchar **lines = g_strsplit(ancestors, "\n", -1);
            for (gsize line = 0; lines[line]; ++line) {
                g_strstrip(lines[line]);
                if (!*lines[line])
                    continue;
                char *ancestor = infiltratorfs_manager_canonical_path(lines[line]);
                if (ancestor && *ancestor)
                    g_hash_table_add(set, ancestor);
                else
                    g_free(ancestor);
            }
            g_strfreev(lines);
        }
        g_free(ancestors);
        g_free(source);
    }
    return set;
}

static int hex_digit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static char *lsblk_unescape(const char *value)
{
    GString *out = g_string_new(NULL);
    if (!out)
        return NULL;
    for (size_t i = 0; value && value[i]; ++i) {
        if (value[i] == '\\' && value[i + 1] == 'x' &&
            value[i + 2] && value[i + 3]) {
            int hi = hex_digit(value[i + 2]);
            int lo = hex_digit(value[i + 3]);
            if (hi >= 0 && lo >= 0) {
                g_string_append_c(out, (char)((hi << 4) | lo));
                i += 3;
                continue;
            }
        }
        g_string_append_c(out, value[i]);
    }
    return g_string_free(out, FALSE);
}

static char *pair_value(gchar **argv, gint argc, const char *key)
{
    size_t key_length = strlen(key);
    for (gint i = 0; i < argc; ++i) {
        if (strncmp(argv[i], key, key_length) == 0 && argv[i][key_length] == '=')
            return lsblk_unescape(argv[i] + key_length + 1u);
    }
    return g_strdup("");
}

static gboolean parent_is_sd(const char *parent)
{
    const char *base = infiltratr_path_basename(parent ? parent : "");
    if (!g_str_has_prefix(base, "mmcblk"))
        return FALSE;
    char path[PATH_MAX];
    if (g_snprintf(path, sizeof(path), "/sys/class/block/%s/device/type", base) >=
        (int)sizeof(path))
        return FALSE;
    char value[32];
    if (!infiltratr_read_text_file(path, value, sizeof(value)))
        return FALSE;
    return strcmp(value, "SD") == 0 || strcmp(value, "SDIO") == 0 ||
           strcmp(value, "SDcombo") == 0;
}

static const char *media_kind(gboolean removable, const char *parent,
                              const char *transport)
{
    if (removable)
        return "Removable";
    if (transport && strcmp(transport, "usb") == 0)
        return "USB";
    if (parent_is_sd(parent))
        return "SD card";
    return "Fixed disk";
}

GPtrArray *discover_partitions(char **error_text)
{
    const char *argv[] = {
        "lsblk", "-P", "-b", "-n", "-p", "-o",
        "PATH,SIZE,TYPE,RM,PKNAME,TRAN,FSTYPE,LABEL,MOUNTPOINTS", NULL
    };
    char *output = NULL;
    char *stderr_text = NULL;
    int status = 0;
    if (!spawn_capture(argv, &output, &stderr_text, &status)) {
        if (error_text)
            *error_text = stderr_text ? stderr_text : g_strdup("Could not enumerate storage devices.");
        else
            g_free(stderr_text);
        return NULL;
    }
    GError *wait_error = NULL;
    if (!g_spawn_check_wait_status(status, &wait_error)) {
        if (error_text)
            *error_text = g_strdup((stderr_text && *stderr_text) ? stderr_text :
                                   (wait_error ? wait_error->message : "Could not enumerate storage devices."));
        g_clear_error(&wait_error);
        g_free(stderr_text);
        g_free(output);
        return NULL;
    }
    g_clear_error(&wait_error);
    g_free(stderr_text);

    GHashTable *protected = protected_devices();
    GPtrArray *targets = g_ptr_array_new_with_free_func(target_free);
    if (!targets) {
        g_hash_table_unref(protected);
        g_free(output);
        return NULL;
    }

    gchar **lines = g_strsplit(output ? output : "", "\n", -1);
    for (gsize line = 0; lines[line]; ++line) {
        if (!*lines[line])
            continue;
        gint argc = 0;
        gchar **pairs = NULL;
        GError *parse_error = NULL;
        if (!g_shell_parse_argv(lines[line], &argc, &pairs, &parse_error)) {
            g_clear_error(&parse_error);
            continue;
        }
        char *type = pair_value(pairs, argc, "TYPE");
        if (strcmp(type, "part") != 0) {
            g_free(type);
            g_strfreev(pairs);
            continue;
        }
        char *path = pair_value(pairs, argc, "PATH");
        char *real = infiltratorfs_manager_canonical_path(path);
        if (!path || !*path || g_hash_table_contains(protected, real)) {
            g_free(real);
            g_free(path);
            g_free(type);
            g_strfreev(pairs);
            continue;
        }
        Target *target = target_new(path, TRUE);
        if (!target) {
            g_free(real);
            g_free(path);
            g_free(type);
            g_strfreev(pairs);
            continue;
        }
        char *size = pair_value(pairs, argc, "SIZE");
        char *rm = pair_value(pairs, argc, "RM");
        char *parent = pair_value(pairs, argc, "PKNAME");
        char *transport = pair_value(pairs, argc, "TRAN");
        char *fstype = pair_value(pairs, argc, "FSTYPE");
        char *label = pair_value(pairs, argc, "LABEL");
        char *mountpoint = pair_value(pairs, argc, "MOUNTPOINTS");
        uint64_t parsed_size = 0;
        if (size && *size)
            (void)infiltratr_parse_u64(size, 10, &parsed_size);
        target->size = parsed_size;
        replace_string(&target->filesystem, fstype);
        replace_string(&target->label, label);
        replace_string(&target->mountpoint, mountpoint);
        replace_string(&target->media,
                       media_kind(rm && strcmp(rm, "1") == 0, parent, transport));
        if (label && *label)
            replace_string(&target->name, label);
        g_ptr_array_add(targets, target);

        g_free(size);
        g_free(rm);
        g_free(parent);
        g_free(transport);
        g_free(fstype);
        g_free(label);
        g_free(mountpoint);
        g_free(real);
        g_free(path);
        g_free(type);
        g_strfreev(pairs);
    }
    g_strfreev(lines);
    g_hash_table_unref(protected);
    g_free(output);
    return targets;
}

char *mountpoint_for(const Target *target)
{
    if (target && target->block)
        return g_strdup_printf("/media/%s/InfiltratorFS", g_get_user_name());
    return g_build_filename(g_get_home_dir(), "InfiltratorFS", NULL);
}

void refresh_mount(Target *target)
{
    if (!target)
        return;
    replace_string(&target->mountpoint, "");
    replace_string(&target->mount_fstype, "");
    if (target->block) {
        const char *argv[] = { "findmnt", "-P", "-rn", "-S", target->path,
                               "-o", "TARGET,FSTYPE", NULL };
        char *output = NULL;
        if (!command_success(argv, &output) || !output) {
            g_free(output);
            return;
        }
        gchar **lines = g_strsplit(output, "\n", 2);
        if (lines[0] && *lines[0]) {
            gint argc = 0;
            gchar **pairs = NULL;
            GError *error = NULL;
            if (g_shell_parse_argv(lines[0], &argc, &pairs, &error)) {
                char *point = pair_value(pairs, argc, "TARGET");
                char *fstype = pair_value(pairs, argc, "FSTYPE");
                replace_string(&target->mountpoint, point);
                replace_string(&target->mount_fstype, fstype);
                g_free(point);
                g_free(fstype);
                g_strfreev(pairs);
            }
            g_clear_error(&error);
        }
        g_strfreev(lines);
        g_free(output);
        return;
    }

    char *point = mountpoint_for(target);
    const char *check[] = { "mountpoint", "-q", point, NULL };
    if (command_success(check, NULL)) {
        replace_string(&target->mountpoint, point);
        const char *findmnt[] = { "findmnt", "-nro", "FSTYPE", "-T", point, NULL };
        char *fstype = NULL;
        if (command_success(findmnt, &fstype) && fstype) {
            g_strstrip(fstype);
            replace_string(&target->mount_fstype, fstype);
        }
        g_free(fstype);
    }
    g_free(point);
}

