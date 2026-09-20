// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include <gtk/gtk.h>

#include "infiltratr/core.h"
#include "infiltratr/design.h"
#include "infiltratr/format.h"
#include "manager/infiltratorfs-manager-contract.h"
#include "infiltratr/posix.h"
#include "infiltratr/posix_path.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef INFILFS_IMPLEMENTATION_VERSION
#define INFILFS_IMPLEMENTATION_VERSION "source/development"
#endif

#define APP_ID "org.infiltratorprojects.InfiltratorFS.Manager"
#define APP_NAME "InfiltratorFS"
#define HELPER "/usr/lib/infiltratorfs/infiltratorfs-manager-helper"
#define TOOL_MKFS "/usr/bin/mkfs.infilfs"
#define TOOL_INSPECT "/usr/bin/infilfs-inspect"
#define TOOL_FSCK "/usr/sbin/fsck.infiltratorfs"
#define TOOL_FORENSIC "/usr/bin/infilfs-forensic"

static const InfiltratrProjectInfo manager_project_info = {
    .struct_size = sizeof(InfiltratrProjectInfo),
    .abi_version = INFILTRATR_PROJECT_INFO_ABI,
    .program_name = APP_NAME,
    .executable_name = "infiltratorfs-manager",
    .application_id = APP_ID,
    .version = INFILFS_IMPLEMENTATION_VERSION,
    .source_id = "Infiltrator-Projects/InfiltratorFS",
    .build_profile = "native",
    .author = "Shannon Smith",
    .website = "https://github.com/Infiltrator-Projects/InfiltratorFS",
    .license_id = "GPL-3.0-or-later",
    .comments =
        "INFILTRATORFS · NATIVE FILESYSTEM\n\n"
        "Native Linux management for InfiltratorFS volumes. Uses the native "
        "VFS/DKMS driver; FUSE is not the product path.",
    .icon_name = "drive-harddisk",
    .copyright_text = "Copyright © 1993-2026 Shannon Smith",
};

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

typedef enum JobAfter {
    JOB_AFTER_NONE = 0,
    JOB_AFTER_FORMAT,
    JOB_AFTER_CREATE_IMAGE,
    JOB_AFTER_MOUNT,
    JOB_AFTER_UNMOUNT
} JobAfter;

typedef struct Manager Manager;

typedef struct Job {
    Manager *manager;
    GPtrArray *commands;
    char *title;
    char *success;
    char *failure;
    char *after_path;
    char *after_label;
    uint64_t after_size;
    JobAfter after;
    gboolean activity;
} Job;

struct Manager {
    GtkApplication *app;
    GtkWidget *window;
    GtkWidget *device_list;
    GtkWidget *count_label;
    GtkWidget *content_stack;
    GtkWidget *page_stack;
    GtkWidget *title_label;
    GtkWidget *path_label;
    GtkWidget *mount_badge;
    GtkWidget *hero_icon;
    GtkWidget *mount_button;
    GtkWidget *unmount_button;
    GtkWidget *format_button;
    GtkWidget *inspect_button;
    GtkWidget *check_button;
    GtkWidget *scrub_button;
    GtkWidget *forensic_button;
    GtkWidget *status_label;
    GtkWidget *version_label;
    GtkWidget *spinner;
    GtkWidget *activity_view;
    GtkTextBuffer *activity_buffer;
    GtkWidget *stat_size;
    GtkWidget *stat_fs;
    GtkWidget *stat_mount;
    GtkWidget *stat_status_card;
    GtkWidget *value_type;
    GtkWidget *value_path;
    GtkWidget *value_fs;
    GtkWidget *value_label;
    GtkWidget *value_mount;
    GtkWidget *value_mountpoint;
    GtkWidget *theme_button;
    GtkCssProvider *css;
    GPtrArray *targets;
    Target *target;
    Target *image_target;
    InfiltratrThemeMode theme_mode;
    gboolean busy;
    gboolean rebuilding;
    gboolean startup_check;
    char *format_device;
};

static void manager_show_target(Manager *manager);
static void manager_refresh_devices(Manager *manager, gboolean preserve);
static void manager_set_enabled(Manager *manager);
static void manager_error(Manager *manager, const char *text);
static void manager_start_format(Manager *manager);

static void target_free(gpointer data)
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

static Target *target_new(const char *path, gboolean block)
{
    Target *target = g_new0(Target, 1);
    if (!target)
        return NULL;
    target->path = g_strdup(path ? path : "");
    target->block = block;
    target->name = g_strdup(infiltratr_path_basename(path ? path : ""));
    target->media = g_strdup(block ? "Fixed disk" : "Image file");
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

static void replace_string(char **slot, const char *value)
{
    char *copy = g_strdup(value ? value : "");
    if (!copy)
        return;
    g_free(*slot);
    *slot = copy;
}

static gboolean spawn_capture(const char *const argv[], char **output,
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

static gboolean command_success(const char *const argv[], char **output)
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

static char *canonical_path(const char *path)
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
        char *real = canonical_path(source);
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
                char *ancestor = canonical_path(lines[line]);
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

static GPtrArray *discover_partitions(char **error_text)
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
        char *real = canonical_path(path);
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

static char *mountpoint_for(const Target *target)
{
    if (target && target->block)
        return g_strdup_printf("/media/%s/InfiltratorFS", g_get_user_name());
    return g_build_filename(g_get_home_dir(), "InfiltratorFS", NULL);
}

static void refresh_mount(Target *target)
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

static GtkWidget *make_label(const char *text, const char *style)
{
    GtkWidget *label = gtk_label_new(text ? text : "");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    if (style)
        gtk_style_context_add_class(gtk_widget_get_style_context(label), style);
    return label;
}

static GtkWidget *make_icon(const char *name, GtkIconSize size, int pixels)
{
    GtkWidget *image = gtk_image_new_from_icon_name(name, size);
    if (pixels > 0)
        gtk_image_set_pixel_size(GTK_IMAGE(image), pixels);
    return image;
}

static void add_class(GtkWidget *widget, const char *style)
{
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), style);
}

static gboolean system_prefers_dark(void)
{
    GtkSettings *settings = gtk_settings_get_default();
    if (!settings)
        return FALSE;
    gboolean prefer = FALSE;
    gchar *theme = NULL;
    g_object_get(settings,
                 "gtk-application-prefer-dark-theme", &prefer,
                 "gtk-theme-name", &theme,
                 NULL);
    gchar *lower = theme ? g_ascii_strdown(theme, -1) : NULL;
    gboolean dark = prefer || (lower && g_strrstr(lower, "dark") != NULL);
    g_free(lower);
    g_free(theme);
    return dark;
}

static void rgb_text(uint32_t rgb, char out[8])
{
    g_snprintf(out, 8, "#%06x", (unsigned)(rgb & 0xffffffu));
}

static void css_replace_u32(GString *css, const char *token, uint32_t value)
{
    char number[16];
    g_snprintf(number, sizeof(number), "%u", (unsigned)value);
    (void)g_string_replace(css, token, number, 0);
}

static void manager_apply_theme(Manager *manager)
{
    const InfiltratrThemePalette *palette = infiltratr_theme_resolve(
        manager->theme_mode, system_prefers_dark());
    const InfiltratrDesignMetrics *metrics = infiltratr_design_metrics();
    const InfiltratrTypography *typography = infiltratr_typography();
    if (!palette || !metrics || !typography)
        return;
    char background[8], panel[8], card[8], surface[8], input[8], border[8];
    char text[8], title[8], muted[8], subtle[8], button_bg[8], button_fg[8];
    char select_bg[8], select_fg[8], neutral[8], fault[8], card_hover[8];
    char surface_hover[8], accent[8], success[8], warning[8], info[8];
    char operation[8], operation_hover[8], titlebar[8], connection[8];
    char connection_border[8], heading[8], summary[8], kicker[8];
    char detail_label[8], note[8], status_border[8], accent_fg[8];
    char accent_hover[8], selected_summary[8], warning_muted[8];
    char warning_border[8], success_border[8];
    rgb_text(palette->background_rgb, background);
    rgb_text(palette->panel_rgb, panel);
    rgb_text(palette->card_rgb, card);
    rgb_text(palette->surface_rgb, surface);
    rgb_text(palette->input_rgb, input);
    rgb_text(palette->border_rgb, border);
    rgb_text(palette->text_rgb, text);
    rgb_text(palette->title_rgb, title);
    rgb_text(palette->muted_rgb, muted);
    rgb_text(palette->subtle_rgb, subtle);
    rgb_text(palette->button_background_rgb, button_bg);
    rgb_text(palette->button_foreground_rgb, button_fg);
    rgb_text(palette->selection_background_rgb, select_bg);
    rgb_text(palette->selection_foreground_rgb, select_fg);
    rgb_text(palette->neutral_accent_rgb, neutral);
    rgb_text(palette->fault_rgb, fault);
    rgb_text(palette->card_hover_rgb, card_hover);
    rgb_text(palette->surface_hover_rgb, surface_hover);
    rgb_text(palette->neutral_accent_rgb, accent);
    rgb_text(palette->success_rgb, success);
    rgb_text(palette->warning_rgb, warning);
    rgb_text(palette->info_rgb, info);
    rgb_text(palette->operation_rgb, operation);
    rgb_text(palette->operation_hover_rgb, operation_hover);
    rgb_text(palette->titlebar_rgb, titlebar);
    rgb_text(palette->connection_rgb, connection);
    rgb_text(palette->connection_border_rgb, connection_border);
    rgb_text(palette->heading_rgb, heading);
    rgb_text(palette->summary_rgb, summary);
    rgb_text(palette->kicker_rgb, kicker);
    rgb_text(palette->detail_label_rgb, detail_label);
    rgb_text(palette->note_rgb, note);
    rgb_text(palette->status_border_rgb, status_border);
    rgb_text(palette->accent_foreground_rgb, accent_fg);
    rgb_text(palette->accent_hover_rgb, accent_hover);
    rgb_text(palette->selected_summary_rgb, selected_summary);
    rgb_text(palette->warning_muted_rgb, warning_muted);
    rgb_text(palette->warning_border_rgb, warning_border);
    rgb_text(palette->success_border_rgb, success_border);

    GString *css = g_string_new(NULL);
    g_string_append_printf(css,
        "* { font-family: '@UI_FONT@'; font-weight: @UI_REGULAR@; }\n"
        "window, dialog, messagedialog, filechooser { background: %s; color: %s; }\n"
        "headerbar { min-height: 44px; background: %s; color: %s; border-bottom: 1px solid %s; }\n"
        "headerbar .title, headerbar label.title { font-family: '@BRAND_FONT@'; font-size: 18px; font-weight: @UI_BOLD@; color: %s; }\n"
        "headerbar .subtitle, headerbar label.subtitle { color: %s; font-size: 12px; font-weight: @UI_BOLD@; }\n"
        "button { min-height: 30px; padding: 0 12px; background: %s; border: 1px solid %s; border-radius: @SMALL_RADIUS@px; font-family: '@UI_FONT@'; font-weight: @UI_BOLD@; }\n"
        "button, button label, button image { color: %s; }\n"
        "headerbar button label, headerbar button image { color: %s; opacity: 1; }\n"
        "button:hover { background: %s; }\n"
        "button:hover label, button:hover image { color: %s; }\n"
        "button:active, button:checked { background: %s; border-color: %s; }\n"
        "button:active, button:active label, button:active image, button:checked, button:checked label, button:checked image { color: %s; }\n"
        "button.suggested-action { background: " "@ACCENT@" "; border-color: " "@ACCENT@" "; }\n"
        "button.suggested-action, button.suggested-action label, button.suggested-action image { color: @ACCENT_FG@; }\n"
        "button.suggested-action:hover { background: @ACCENT_HOVER@; border-color: @ACCENT_HOVER@; }\n"
        "button.destructive-action { background: %s; border-color: %s; }\n"
        "button.destructive-action, button.destructive-action label, button.destructive-action image { color: %s; }\n"
        "entry, spinbutton, textview, textview text { background: %s; color: %s; border-color: %s; caret-color: " "@ACCENT@" "; }\n"
        "stackswitcher button { background: transparent; color: %s; border-color: transparent; border-radius: 0; padding: 0 14px; }\n"
        "stackswitcher button:checked { color: %s; border-bottom: 2px solid " "@ACCENT@" "; }\n"
        ".sidebar { background: %s; border-right: 1px solid %s; }\n"
        ".sidebar-title { font-size: 11px; font-weight: @UI_BOLD@; color: %s; }\n"
        ".sidebar-count { color: %s; font-size: 11px; }\n"
        ".device-list { background: transparent; }\n"
        ".device-list row { border: 1px solid transparent; border-radius: @SMALL_RADIUS@px; margin: 3px 10px; }\n"
        ".device-list row:hover { background: %s; }\n"
        ".device-list row:selected, .device-list row:selected:hover { background: %s; border-color: %s; border-left-width: 3px; border-left-color: " "@ACCENT@" "; }\n"
        ".device-row { padding: 11px 12px; }\n"
        ".device-name { font-size: 14px; font-weight: @UI_BOLD@; color: %s; }\n"
        ".device-meta { color: %s; font-size: 12px; }\n"
        ".device-list row:selected .device-meta { color: @SELECTED_SUMMARY@; }\n"
        ".content { padding: 30px 34px 24px 34px; }\n"
        ".hero-title { color: %s; font-family: '@BRAND_FONT@'; font-size: 27px; font-weight: @BRAND_WEIGHT@; }\n"
        ".hero-path { color: %s; font-size: 12px; }\n"
        ".badge { padding: 4px 9px; border-radius: 999px; background: %s; border: 1px solid %s; color: %s; font-size: 11px; font-weight: @UI_BOLD@; }\n"
        ".badge-mounted { border-color: " "@ACCENT@" "; color: " "@ACCENT@" "; }\n"
        ".section-title { color: %s; font-size: 16px; font-weight: @UI_BOLD@; }\n"
        ".section-subtitle { color: %s; font-size: 12px; }\n"
        ".card, .stat-card, .empty-state { border: 1px solid %s; background: %s; }\n"
        ".card { padding: 18px; border-radius: @CONTROL_RADIUS@px; }\n"
        ".stat-card { padding: 15px 16px; border-radius: @CONTROL_RADIUS@px; }\n"
        ".stat-caption { color: %s; font-size: 10px; font-weight: @UI_BOLD@; }\n"
        ".stat-value { color: %s; font-size: 17px; font-weight: @UI_BOLD@; }\n"
        ".detail-caption { color: %s; font-size: 11px; }\n"
        ".detail-value { color: %s; font-weight: @UI_BOLD@; }\n"
        ".action-row { padding: 12px; border-radius: @SMALL_RADIUS@px; }\n"
        ".action-row:hover { background: %s; }\n"
        ".action-title { color: %s; font-weight: @UI_BOLD@; }\n"
        ".action-description { color: %s; font-size: 12px; }\n"
        ".danger-zone { padding: 16px; border: 1px solid %s; border-radius: @CONTROL_RADIUS@px; background: %s; }\n"
        ".empty-state { min-width: 460px; padding: 36px 46px; border-radius: @CARD_RADIUS@px; }\n"
        ".empty-title { color: %s; font-family: '@BRAND_FONT@'; font-size: 30px; font-weight: @BRAND_WEIGHT@; }\n"
        ".empty-copy { color: %s; font-size: 13px; }\n"
        ".activity-frame { border: 1px solid %s; border-radius: @CONTROL_RADIUS@px; background: %s; }\n"
        ".activity, .activity text { background: %s; color: %s; font-family: '@UI_FONT@'; font-weight: @UI_REGULAR@; }\n"
        ".statusbar { padding: 8px 12px; border-top: 1px solid %s; background: %s; }\n"
        ".status-text { color: %s; font-size: 11px; }\n"
        ".link-about-dialog { background: %s; color: %s; }\n",
        background, text, titlebar, title, border, title, summary,
        button_bg, border, button_fg, button_fg, neutral, button_fg,
        select_bg, "@ACCENT@", select_fg, surface, fault, fault,
        input, text, border, summary, heading, panel, border, kicker, summary,
        card_hover, select_bg, border, text, summary, title, summary,
        surface, status_border, warning_muted, heading, summary, border, card,
        kicker, heading, detail_label, text, surface_hover, heading, note,
        fault, surface, title, note, border, input, input, text,
        connection_border, connection, summary, background, text);

    /*
     * Common 1.19.10 exposes the complete Linux MBLINK-derived appearance roles in addition
     * to the base surfaces.  Use those roles directly instead of flattening
     * the Manager into neutral accent + fault only.
     */
    g_string_append_printf(css,
        ".badge { border-color: %s; color: %s; }\n"
        ".badge-mounted { border-color: %s; color: %s; }\n"
        ".section-info { color: %s; }\n"
        ".section-maintenance { color: %s; }\n"
        ".section-danger { color: %s; }\n"
        ".card-info { border-color: %s; }\n"
        ".card-maintenance { border-color: %s; }\n"
        ".stat-capacity { border-color: %s; }\n"
        ".stat-filesystem { border-color: %s; }\n"
        ".stat-status { border-color: %s; }\n"
        ".stat-status .stat-value { color: %s; }\n"
        ".stat-status.status-mounted { border-color: %s; }\n"
        ".stat-status.status-mounted .stat-value { color: %s; }\n"
        ".action-row button { background: %s; border-color: %s; }\n"
        ".action-row button, .action-row button label, .action-row button image { color: %s; }\n"
        ".action-row button:hover { background: %s; }\n"
        ".action-row button:hover, .action-row button:hover label, .action-row button:hover image { color: %s; }\n"
        ".action-inspect image { color: %s; }\n"
        ".action-check image { color: %s; }\n"
        ".action-scrub image { color: %s; }\n"
        ".action-forensic image { color: %s; }\n"
        ".danger-zone .section-subtitle { color: %s; }\n",
        warning_border, warning_muted, success_border, success,
        info, accent, fault,
        info, accent,
        info, accent, warning_border, warning_muted, success_border, success,
        operation, border, text, operation_hover, text,
        info, success, warning, accent, fault);

    (void)g_string_replace(css, "@UI_FONT@", typography->ui_family, 0);
    (void)g_string_replace(css, "@BRAND_FONT@", typography->brand_family, 0);
    (void)g_string_replace(css, "@ACCENT@", accent, 0);
    (void)g_string_replace(css, "@ACCENT_FG@", accent_fg, 0);
    (void)g_string_replace(css, "@ACCENT_HOVER@", accent_hover, 0);
    (void)g_string_replace(css, "@SELECTED_SUMMARY@", selected_summary, 0);
    css_replace_u32(css, "@UI_REGULAR@", typography->ui_regular_weight);
    css_replace_u32(css, "@UI_BOLD@", typography->ui_bold_weight);
    css_replace_u32(css, "@BRAND_WEIGHT@", typography->brand_weight);
    css_replace_u32(css, "@SMALL_RADIUS@", metrics->small_radius);
    css_replace_u32(css, "@CONTROL_RADIUS@", metrics->control_radius);
    css_replace_u32(css, "@CARD_RADIUS@", metrics->card_radius);

    if (!manager->css) {
        manager->css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(), GTK_STYLE_PROVIDER(manager->css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    GError *error = NULL;
    gtk_css_provider_load_from_data(manager->css, css->str, -1, &error);
    if (error) {
        g_warning("InfiltratorFS theme CSS: %s", error->message);
        g_clear_error(&error);
    }
    g_string_free(css, TRUE);

    if (manager->theme_button) {
        char text_value[64];
        g_snprintf(text_value, sizeof(text_value), "Theme: %s",
                   infiltratr_theme_mode_name(manager->theme_mode));
        gtk_button_set_label(GTK_BUTTON(manager->theme_button), text_value);
    }
}

static char *theme_config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "infiltratorfs", "theme", NULL);
}

static InfiltratrThemeMode load_theme_mode(void)
{
    char *path = theme_config_path();
    char value[32] = {0};
    InfiltratrThemeMode mode = INFILTRATR_THEME_SYSTEM;
    if (path && infiltratr_read_text_file(path, value, sizeof(value))) {
        if (g_ascii_strcasecmp(value, "day") == 0)
            mode = INFILTRATR_THEME_DAY;
        else if (g_ascii_strcasecmp(value, "night") == 0)
            mode = INFILTRATR_THEME_NIGHT;
    }
    g_free(path);
    return mode;
}

static void save_theme_mode(InfiltratrThemeMode mode)
{
    char *path = theme_config_path();
    char *directory = path ? g_path_get_dirname(path) : NULL;
    if (!path || !directory) {
        g_free(path);
        g_free(directory);
        return;
    }
    if (g_mkdir_with_parents(directory, 0700) == 0) {
        const char *name = infiltratr_theme_mode_name(mode);
        char lower[16];
        gsize length = strlen(name);
        if (length < sizeof(lower) - 2u) {
            for (gsize i = 0; i < length; ++i)
                lower[i] = (char)g_ascii_tolower(name[i]);
            lower[length++] = '\n';
            lower[length] = '\0';
            (void)infiltratr_atomic_file_write_bytes(
                path, INFILTRATR_ATOMIC_FILE_PRIVATE, lower, length);
        }
    }
    g_free(directory);
    g_free(path);
}

static void activity_append(Manager *manager, const char *text)
{
    if (!manager || !manager->activity_buffer || !text)
        return;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(manager->activity_buffer, &end);
    gtk_text_buffer_insert(manager->activity_buffer, &end, text, -1);
    GtkTextMark *mark = gtk_text_buffer_create_mark(
        manager->activity_buffer, NULL, &end, FALSE);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(manager->activity_view), mark);
    gtk_text_buffer_delete_mark(manager->activity_buffer, mark);
}

typedef struct LogMessage {
    Manager *manager;
    char *text;
} LogMessage;

static gboolean activity_append_idle(gpointer data)
{
    LogMessage *message = data;
    activity_append(message->manager, message->text);
    g_free(message->text);
    g_free(message);
    return G_SOURCE_REMOVE;
}

static void queue_log(Manager *manager, const char *text)
{
    LogMessage *message = g_new0(LogMessage, 1);
    if (!message)
        return;
    message->manager = manager;
    message->text = g_strdup(text ? text : "");
    if (!message->text) {
        g_free(message);
        return;
    }
    g_idle_add(activity_append_idle, message);
}

static void job_free(Job *job)
{
    if (!job)
        return;
    if (job->commands)
        g_ptr_array_free(job->commands, TRUE);
    g_free(job->title);
    g_free(job->success);
    g_free(job->failure);
    g_free(job->after_path);
    g_free(job->after_label);
    g_free(job);
}

static Job *job_new(Manager *manager, const char *title, const char *success,
                    JobAfter after, gboolean activity)
{
    Job *job = g_new0(Job, 1);
    if (!job)
        return NULL;
    job->manager = manager;
    job->title = g_strdup(title);
    job->success = g_strdup(success);
    job->after = after;
    job->activity = activity;
    job->commands = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
    if (!job->title || !job->success || !job->commands) {
        job_free(job);
        return NULL;
    }
    return job;
}

static void job_add_command(Job *job, const char *const argv[])
{
    if (!job || !job->commands || !argv)
        return;
    g_ptr_array_add(job->commands, g_strdupv((gchar **)argv));
}

static char *command_display(gchar **argv)
{
    GString *display = g_string_new("$ ");
    for (gint i = 0; argv && argv[i]; ++i) {
        char *quoted = g_shell_quote(argv[i]);
        if (i)
            g_string_append_c(display, ' ');
        g_string_append(display, quoted);
        g_free(quoted);
    }
    g_string_append_c(display, '\n');
    return g_string_free(display, FALSE);
}

static void open_in_files(const char *path)
{
    if (!path || !*path)
        return;
    gchar *argv[] = { "xdg-open", (gchar *)path, NULL };
    GError *error = NULL;
    (void)g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                        NULL, NULL, NULL, &error);
    g_clear_error(&error);
}

static gboolean job_finish_idle(gpointer data)
{
    Job *job = data;
    Manager *manager = job->manager;
    manager->busy = FALSE;
    gtk_spinner_stop(GTK_SPINNER(manager->spinner));
    gtk_widget_hide(manager->spinner);

    if (job->failure && *job->failure) {
        gtk_label_set_text(GTK_LABEL(manager->status_label), "Operation failed");
        activity_append(manager, "ERROR: ");
        activity_append(manager, job->failure);
        activity_append(manager, "\n");
        manager_error(manager, job->failure);
    } else {
        gtk_label_set_text(GTK_LABEL(manager->status_label), job->success);
        activity_append(manager, job->success);
        activity_append(manager, "\n");
        if (job->after == JOB_AFTER_CREATE_IMAGE) {
            target_free(manager->image_target);
            manager->image_target = target_new(job->after_path, FALSE);
            if (manager->image_target) {
                manager->image_target->size = job->after_size;
                replace_string(&manager->image_target->filesystem, "infiltratorfs");
                replace_string(&manager->image_target->label, job->after_label);
                replace_string(&manager->image_target->name, job->after_label);
                manager->target = manager->image_target;
            }
        } else if (job->after == JOB_AFTER_FORMAT && manager->target) {
            replace_string(&manager->target->filesystem, "infiltratorfs");
            replace_string(&manager->target->label, job->after_label);
            replace_string(&manager->target->name, job->after_label);
        } else if (job->after == JOB_AFTER_UNMOUNT && manager->target) {
            replace_string(&manager->target->mountpoint, "");
            replace_string(&manager->target->mount_fstype, "");
        }
        manager_refresh_devices(manager, TRUE);
        manager_show_target(manager);
        if (job->after == JOB_AFTER_MOUNT && manager->target) {
            refresh_mount(manager->target);
            if (manager->target->mountpoint && *manager->target->mountpoint)
                open_in_files(manager->target->mountpoint);
            else {
                char *point = mountpoint_for(manager->target);
                open_in_files(point);
                g_free(point);
            }
        }
        if (manager->page_stack)
            gtk_stack_set_visible_child_name(GTK_STACK(manager->page_stack), "overview");
    }
    manager_set_enabled(manager);
    job_free(job);
    return G_SOURCE_REMOVE;
}

static gpointer job_worker(gpointer data)
{
    Job *job = data;
    for (guint i = 0; i < job->commands->len; ++i) {
        gchar **argv = g_ptr_array_index(job->commands, i);
        char *display = command_display(argv);
        queue_log(job->manager, display);
        g_free(display);

        char *output = NULL;
        char *stderr_text = NULL;
        int wait_status = 0;
        gboolean spawned = spawn_capture((const char *const *)argv,
                                         &output, &stderr_text, &wait_status);
        if (output && *output)
            queue_log(job->manager, output);
        if (stderr_text && *stderr_text)
            queue_log(job->manager, stderr_text);
        if (!spawned) {
            job->failure = g_strdup((stderr_text && *stderr_text) ? stderr_text :
                                    "Could not start command.");
        } else {
            GError *error = NULL;
            if (!g_spawn_check_wait_status(wait_status, &error)) {
                if (stderr_text && *stderr_text)
                    job->failure = g_strdup(g_strstrip(stderr_text));
                else if (output && *output)
                    job->failure = g_strdup(g_strstrip(output));
                else
                    job->failure = g_strdup(error ? error->message : "Command failed.");
            }
            g_clear_error(&error);
        }
        g_free(output);
        g_free(stderr_text);
        if (job->failure)
            break;
    }
    g_idle_add(job_finish_idle, job);
    return NULL;
}

static void manager_run_job(Manager *manager, Job *job)
{
    if (!manager || !job || manager->busy) {
        job_free(job);
        return;
    }
    if (job->activity)
        gtk_stack_set_visible_child_name(GTK_STACK(manager->page_stack), "activity");
    char heading[256];
    g_snprintf(heading, sizeof(heading), "\n── %s ──\n", job->title);
    activity_append(manager, heading);
    manager->busy = TRUE;
    gtk_widget_show(manager->spinner);
    gtk_spinner_start(GTK_SPINNER(manager->spinner));
    char status[256];
    g_snprintf(status, sizeof(status), "%s…", job->title);
    gtk_label_set_text(GTK_LABEL(manager->status_label), status);
    manager_set_enabled(manager);
    GThread *thread = g_thread_new("infiltratorfs-job", job_worker, job);
    if (thread)
        g_thread_unref(thread);
    else {
        manager->busy = FALSE;
        manager_error(manager, "Could not create worker thread.");
        job_free(job);
    }
}

static void manager_error(Manager *manager, const char *text)
{
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(manager->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", "Operation failed");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s",
                                             text ? text : "Unknown error.");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void manager_set_enabled(Manager *manager)
{
    const gboolean mounted =
        manager->target && manager->target->mountpoint &&
        *manager->target->mountpoint;
    const gboolean is_infiltrator =
        manager->target && manager->target->filesystem &&
        g_ascii_strcasecmp(manager->target->filesystem, "infiltratorfs") == 0;
    const struct infilfs_manager_state state = {
        .has_target = manager->target != NULL,
        .is_infiltrator = is_infiltrator,
        .mounted = mounted,
        .busy = manager->busy,
        .volume_open = mounted
    };
    struct infilfs_manager_enablement enabled;
    infilfs_manager_compute_enablement(&state, &enabled);

    GtkWidget *buttons[] = {
        manager->inspect_button, manager->check_button,
        manager->scrub_button, manager->forensic_button, NULL
    };
    for (size_t i = 0; buttons[i]; ++i)
        gtk_widget_set_sensitive(buttons[i], enabled.maintenance);
    gtk_widget_set_sensitive(manager->format_button, enabled.format);
    gtk_widget_set_sensitive(manager->mount_button, enabled.mount);
    gtk_widget_set_sensitive(manager->unmount_button, enabled.unmount);
    gtk_button_set_label(GTK_BUTTON(manager->mount_button),
                         mounted ? "Open in Files" :
                         infilfs_manager_copy()->mount_button);
}

static void set_detail(GtkWidget *label, const char *text)
{
    gtk_label_set_text(GTK_LABEL(label), (text && *text) ? text : "—");
}

static void inspect_image_identity(Target *target)
{
    if (!target || target->block)
        return;
    struct stat st;
    target->size = stat(target->path, &st) == 0 ? (uint64_t)st.st_size : 0;
    const char *argv[] = { TOOL_INSPECT, "--udev", target->path, NULL };
    char *output = NULL;
    if (!command_success(argv, &output) || !output) {
        g_free(output);
        return;
    }
    gchar **lines = g_strsplit(output, "\n", -1);
    for (gsize i = 0; lines[i]; ++i) {
        if (g_str_has_prefix(lines[i], "ID_FS_TYPE="))
            replace_string(&target->filesystem, lines[i] + strlen("ID_FS_TYPE="));
        else if (g_str_has_prefix(lines[i], "ID_FS_LABEL=")) {
            replace_string(&target->label, lines[i] + strlen("ID_FS_LABEL="));
            if (*target->label)
                replace_string(&target->name, target->label);
        }
    }
    g_strfreev(lines);
    g_free(output);
}

static void manager_show_target(Manager *manager)
{
    if (!manager->target) {
        gtk_stack_set_visible_child_name(GTK_STACK(manager->content_stack), "empty");
        gtk_label_set_text(GTK_LABEL(manager->status_label), "Ready");
        manager_set_enabled(manager);
        return;
    }
    Target *target = manager->target;
    refresh_mount(target);
    inspect_image_identity(target);
    gtk_stack_set_visible_child_name(GTK_STACK(manager->content_stack), "target");
    gtk_label_set_text(GTK_LABEL(manager->title_label),
                       (target->label && *target->label) ? target->label : target->name);
    gtk_label_set_text(GTK_LABEL(manager->path_label), target->path);
    gtk_image_set_from_icon_name(GTK_IMAGE(manager->hero_icon),
        target->block ? "drive-harddisk-symbolic" : "document-open-symbolic",
        GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(manager->hero_icon), 32);

    gboolean mounted = target->mountpoint && *target->mountpoint;
    gtk_label_set_text(GTK_LABEL(manager->mount_badge),
                       mounted ? "Mounted" : "Not mounted");
    GtkStyleContext *badge = gtk_widget_get_style_context(manager->mount_badge);
    if (mounted)
        gtk_style_context_add_class(badge, "badge-mounted");
    else
        gtk_style_context_remove_class(badge, "badge-mounted");
    if (manager->stat_status_card) {
        GtkStyleContext *status_card =
            gtk_widget_get_style_context(manager->stat_status_card);
        if (mounted)
            gtk_style_context_add_class(status_card, "status-mounted");
        else
            gtk_style_context_remove_class(status_card, "status-mounted");
    }

    char size_text[64];
    (void)infilfs_manager_format_capacity(target->size, size_text, sizeof(size_text));
    gtk_label_set_text(GTK_LABEL(manager->stat_size), size_text);
    gtk_label_set_text(GTK_LABEL(manager->stat_fs),
        (target->filesystem && *target->filesystem) ? target->filesystem : "Unknown / unformatted");
    gtk_label_set_text(GTK_LABEL(manager->stat_mount), mounted ? "Mounted" : "Not mounted");
    set_detail(manager->value_type, target->block ? target->media : "Image file");
    set_detail(manager->value_path, target->path);
    set_detail(manager->value_fs,
        (target->filesystem && *target->filesystem) ? target->filesystem : "Unknown / unformatted");
    set_detail(manager->value_label, target->label);
    char mount_state[256];
    if (mounted && target->mount_fstype && *target->mount_fstype)
        g_snprintf(mount_state, sizeof(mount_state), "Mounted (%s)", target->mount_fstype);
    else
        g_strlcpy(mount_state, mounted ? "Mounted" : "Not mounted", sizeof(mount_state));
    set_detail(manager->value_mount, mount_state);
    set_detail(manager->value_mountpoint, target->mountpoint);
    manager_set_enabled(manager);
}

static GtkWidget *device_row(Target *target)
{
    GtkWidget *row = gtk_list_box_row_new();
    g_object_set_data(G_OBJECT(row), "infiltratorfs-target", target);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 11);
    add_class(box, "device-row");
    GtkWidget *icon = make_icon(
        target->media && strcmp(target->media, "Fixed disk") != 0 ?
        "drive-removable-media-symbolic" : "drive-harddisk-symbolic",
        GTK_ICON_SIZE_DIALOG, 28);
    gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
    GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *name = make_label(target->name, "device-name");
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    char size_text[64];
    (void)infilfs_manager_format_capacity(target->size, size_text, sizeof(size_text));
    char *meta_text = g_strdup_printf("%s  •  %s", target->path, size_text);
    GtkWidget *meta = make_label(meta_text, "device-meta");
    g_free(meta_text);
    gtk_label_set_ellipsize(GTK_LABEL(meta), PANGO_ELLIPSIZE_END);
    char *info_text = (target->filesystem && *target->filesystem) ?
        g_strdup_printf("%s  •  %s", target->media, target->filesystem) :
        g_strdup(target->media);
    GtkWidget *info = make_label(info_text, "device-meta");
    g_free(info_text);
    gtk_box_pack_start(GTK_BOX(labels), name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(labels), meta, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(labels), info, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), labels, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(row), box);
    return row;
}

static void manager_refresh_devices(Manager *manager, gboolean preserve)
{
    char *selected = NULL;
    gboolean preserve_image = preserve && manager->target && !manager->target->block;
    if (preserve && manager->target && manager->target->block)
        selected = g_strdup(manager->target->path);

    manager->rebuilding = TRUE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(manager->device_list));
    for (GList *node = children; node; node = node->next)
        gtk_widget_destroy(GTK_WIDGET(node->data));
    g_list_free(children);
    if (manager->targets)
        g_ptr_array_free(manager->targets, TRUE);
    manager->targets = NULL;
    if (!preserve_image)
        manager->target = NULL;

    char *error_text = NULL;
    manager->targets = discover_partitions(&error_text);
    if (!manager->targets) {
        manager->targets = g_ptr_array_new_with_free_func(target_free);
        manager->rebuilding = FALSE;
        manager_error(manager, error_text ? error_text : "Could not enumerate storage devices.");
        g_free(error_text);
        g_free(selected);
        return;
    }
    g_free(error_text);

    GtkListBoxRow *select_row = NULL;
    for (guint i = 0; i < manager->targets->len; ++i) {
        Target *target = g_ptr_array_index(manager->targets, i);
        GtkWidget *row = device_row(target);
        gtk_container_add(GTK_CONTAINER(manager->device_list), row);
        if (selected && strcmp(selected, target->path) == 0)
            select_row = GTK_LIST_BOX_ROW(row);
    }
    char count[96];
    g_snprintf(count, sizeof(count), "%u available partition%s",
               manager->targets->len, manager->targets->len == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(manager->count_label), count);
    gtk_widget_show_all(manager->device_list);
    manager->rebuilding = FALSE;
    if (select_row)
        gtk_list_box_select_row(GTK_LIST_BOX(manager->device_list), select_row);
    else if (preserve_image)
        manager_show_target(manager);
    else if (selected)
        manager_show_target(manager);
    g_free(selected);
}

static void on_device_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    (void)box;
    Manager *manager = data;
    if (!row || manager->rebuilding)
        return;
    manager->target = g_object_get_data(G_OBJECT(row), "infiltratorfs-target");
    manager_show_target(manager);
}

static gboolean require_offline(Manager *manager)
{
    if (!manager->target)
        return FALSE;
    manager_show_target(manager);
    if (manager->target->mountpoint && *manager->target->mountpoint) {
        manager_error(manager,
            "Unmount the volume before running inspect, verify, or forensic maintenance. "
            "These tools examine a stable on-disk checkpoint and must not race a live mount.");
        return FALSE;
    }
    return TRUE;
}

static void run_maintenance(Manager *manager, const char *title,
                            const char *success, const char *block_op,
                            const char *tool, const char *tool_arg)
{
    if (!require_offline(manager))
        return;
    Job *job = job_new(manager, title, success, JOB_AFTER_NONE, TRUE);
    if (!job)
        return;
    if (manager->target->block) {
        const char *argv[] = { "pkexec", HELPER, block_op,
                               manager->target->path, NULL };
        job_add_command(job, argv);
    } else if (tool_arg) {
        const char *argv[] = { tool, tool_arg, manager->target->path, NULL };
        job_add_command(job, argv);
    } else {
        const char *argv[] = { tool, manager->target->path, NULL };
        job_add_command(job, argv);
    }
    manager_run_job(manager, job);
}

static void run_shared_maintenance(
    Manager *manager, enum infilfs_manager_action_id id,
    const char *block_op, const char *tool, const char *tool_arg)
{
    const struct infilfs_manager_action_descriptor *action =
        infilfs_manager_action(id);
    if (!action)
        return;
    run_maintenance(manager, action->title, action->success,
                    block_op, tool, tool_arg);
}

static void on_inspect(GtkButton *button, gpointer data)
{
    (void)button;
    run_shared_maintenance(data, INFILFS_MANAGER_ACTION_INSPECT,
                           "inspect-block", TOOL_INSPECT, NULL);
}

static void on_check(GtkButton *button, gpointer data)
{
    (void)button;
    run_shared_maintenance(data, INFILFS_MANAGER_ACTION_CHECK,
                           "check-block", TOOL_FSCK, NULL);
}

static void on_scrub(GtkButton *button, gpointer data)
{
    (void)button;
    run_shared_maintenance(data, INFILFS_MANAGER_ACTION_SCRUB,
                           "scrub-block", TOOL_FSCK, "--scrub");
}

static void on_forensic(GtkButton *button, gpointer data)
{
    (void)button;
    run_shared_maintenance(data, INFILFS_MANAGER_ACTION_FORENSIC,
                           "forensic-block", TOOL_FORENSIC, NULL);
}

static char *ask_label(Manager *manager)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Format volume", GTK_WINDOW(manager->window), GTK_DIALOG_MODAL,
        "Cancel", GTK_RESPONSE_CANCEL, "Continue", GTK_RESPONSE_OK, NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(area), 20);
    gtk_box_set_spacing(GTK_BOX(area), 12);
    gtk_box_pack_start(GTK_BOX(area), make_label("Volume label", "section-title"),
                       FALSE, FALSE, 0);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_entry_set_text(GTK_ENTRY(entry),
        manager->target && manager->target->label && *manager->target->label ?
        manager->target->label : "InfiltratorFS");
    gtk_box_pack_start(GTK_BOX(area), entry, FALSE, FALSE, 0);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_widget_show_all(dialog);
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    char *label = NULL;
    if (response == GTK_RESPONSE_OK) {
        const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
        char *copy = g_strdup(text ? text : "");
        if (copy) {
            g_strstrip(copy);
            if (*copy)
                label = copy;
            else
                g_free(copy);
        }
    }
    gtk_widget_destroy(dialog);
    return label;
}

static void manager_start_format(Manager *manager)
{
    if (!manager->target)
        return;
    manager_show_target(manager);
    if (manager->target->mountpoint && *manager->target->mountpoint) {
        manager_error(manager, "Unmount the selected volume before formatting it.");
        return;
    }
    char *label = ask_label(manager);
    if (!label)
        return;
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(manager->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s", "Erase and format this volume?");
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dialog),
        "All existing data on %s will be permanently destroyed.\n\nNew volume label: %s",
        manager->target->path, label);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    GtkWidget *erase = gtk_dialog_add_button(GTK_DIALOG(dialog),
                                             "Erase and Format", GTK_RESPONSE_ACCEPT);
    add_class(erase, "destructive-action");
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if (response != GTK_RESPONSE_ACCEPT) {
        g_free(label);
        return;
    }
    Job *job = job_new(manager, "Format volume", "Formatting completed successfully.",
                       JOB_AFTER_FORMAT, TRUE);
    if (!job) {
        g_free(label);
        return;
    }
    job->after_label = g_strdup(label);
    if (manager->target->block) {
        const char *argv[] = { "pkexec", HELPER, "format-block",
                               manager->target->path, label, NULL };
        job_add_command(job, argv);
    } else {
        const char *argv[] = { TOOL_MKFS, "-L", label, manager->target->path, NULL };
        job_add_command(job, argv);
    }
    g_free(label);
    manager_run_job(manager, job);
}

static void on_format(GtkButton *button, gpointer data)
{
    (void)button;
    manager_start_format(data);
}

static void on_mount(GtkButton *button, gpointer data)
{
    (void)button;
    Manager *manager = data;
    if (!manager->target)
        return;
    manager_show_target(manager);
    if (manager->target->mountpoint && *manager->target->mountpoint) {
        open_in_files(manager->target->mountpoint);
        return;
    }
    char *point = mountpoint_for(manager->target);
    char uid[32], gid[32];
    g_snprintf(uid, sizeof(uid), "%u", (unsigned)getuid());
    g_snprintf(gid, sizeof(gid), "%u", (unsigned)getgid());
    const char *op = manager->target->block ? "mount-block" : "mount-image";
    const char *argv[] = { "pkexec", HELPER, op, manager->target->path,
                           uid, gid, point, NULL };
    Job *job = job_new(manager, "Mount filesystem", "Filesystem mounted natively.",
                       JOB_AFTER_MOUNT, FALSE);
    if (job)
        job_add_command(job, argv);
    g_free(point);
    manager_run_job(manager, job);
}

static void on_unmount(GtkButton *button, gpointer data)
{
    (void)button;
    Manager *manager = data;
    if (!manager->target)
        return;
    char *point = (manager->target->mountpoint && *manager->target->mountpoint) ?
        g_strdup(manager->target->mountpoint) : mountpoint_for(manager->target);
    char uid[32];
    g_snprintf(uid, sizeof(uid), "%u", (unsigned)getuid());
    Job *job = job_new(manager, "Unmount filesystem", "Filesystem unmounted safely.",
                       JOB_AFTER_UNMOUNT, FALSE);
    if (job) {
        if (manager->target->block) {
            const char *argv[] = { "pkexec", HELPER, "unmount-block",
                                   manager->target->path, uid, point, NULL };
            job_add_command(job, argv);
        } else {
            const char *argv[] = { "pkexec", HELPER, "unmount-image",
                                   uid, point, NULL };
            job_add_command(job, argv);
        }
    }
    g_free(point);
    manager_run_job(manager, job);
}

static void on_open_image(GtkButton *button, gpointer data)
{
    (void)button;
    Manager *manager = data;
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Open InfiltratorFS image", GTK_WINDOW(manager->window),
        GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel", GTK_RESPONSE_CANCEL,
        "Open Image", GTK_RESPONSE_OK, NULL);
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    char *filename = response == GTK_RESPONSE_OK ?
        gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog)) : NULL;
    gtk_widget_destroy(dialog);
    if (!filename)
        return;
    struct stat st;
    if (stat(filename, &st) != 0 || !S_ISREG(st.st_mode)) {
        manager_error(manager, "Select a regular image file.");
        g_free(filename);
        return;
    }
    target_free(manager->image_target);
    manager->image_target = target_new(filename, FALSE);
    if (manager->image_target) {
        manager->image_target->size = (uint64_t)st.st_size;
        manager->target = manager->image_target;
        gtk_list_box_unselect_all(GTK_LIST_BOX(manager->device_list));
        manager_show_target(manager);
        gtk_stack_set_visible_child_name(GTK_STACK(manager->page_stack), "overview");
    }
    g_free(filename);
}

static void on_create_image(GtkButton *button, gpointer data)
{
    (void)button;
    Manager *manager = data;
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Create InfiltratorFS image", GTK_WINDOW(manager->window),
        GTK_FILE_CHOOSER_ACTION_SAVE, "Cancel", GTK_RESPONSE_CANCEL,
        "Continue", GTK_RESPONSE_OK, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(chooser), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser), "infiltratorfs.img");
    gint response = gtk_dialog_run(GTK_DIALOG(chooser));
    char *filename = response == GTK_RESPONSE_OK ?
        gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser)) : NULL;
    gtk_widget_destroy(chooser);
    if (!filename)
        return;

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "New filesystem image", GTK_WINDOW(manager->window), GTK_DIALOG_MODAL,
        "Cancel", GTK_RESPONSE_CANCEL, "Create Image", GTK_RESPONSE_OK, NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(area), 20);
    gtk_box_set_spacing(GTK_BOX(area), 14);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    GtkWidget *size = gtk_spin_button_new_with_range(16, 1048576, 16);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(size), 512);
    GtkWidget *label = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(label), "InfiltratorFS");
    gtk_grid_attach(GTK_GRID(grid), make_label("Capacity (MiB)", NULL), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), size, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_label("Volume label", NULL), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), label, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(area), grid, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);
    response = gtk_dialog_run(GTK_DIALOG(dialog));
    gint mib = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(size));
    char *fslabel = g_strdup(gtk_entry_get_text(GTK_ENTRY(label)));
    gtk_widget_destroy(dialog);
    if (response != GTK_RESPONSE_OK || !fslabel) {
        g_free(filename);
        g_free(fslabel);
        return;
    }
    g_strstrip(fslabel);
    if (!*fslabel) {
        manager_error(manager, "The volume label cannot be empty.");
        g_free(filename);
        g_free(fslabel);
        return;
    }
    char size_arg[64];
    g_snprintf(size_arg, sizeof(size_arg), "%dM", mib);
    const char *truncate_argv[] = { "truncate", "-s", size_arg, "--", filename, NULL };
    const char *mkfs_argv[] = { TOOL_MKFS, "-L", fslabel, filename, NULL };
    Job *job = job_new(manager, "Create image", "Image created and formatted successfully.",
                       JOB_AFTER_CREATE_IMAGE, TRUE);
    if (job) {
        job->after_path = g_strdup(filename);
        job->after_label = g_strdup(fslabel);
        job->after_size = (uint64_t)mib * UINT64_C(1024) * UINT64_C(1024);
        job_add_command(job, truncate_argv);
        job_add_command(job, mkfs_argv);
    }
    g_free(filename);
    g_free(fslabel);
    manager_run_job(manager, job);
}

static void on_theme(GtkButton *button, gpointer data)
{
    (void)button;
    Manager *manager = data;
    manager->theme_mode = infiltratr_theme_mode_next(manager->theme_mode);
    save_theme_mode(manager->theme_mode);
    manager_apply_theme(manager);
}

static void on_system_theme_changed(GObject *object, GParamSpec *pspec, gpointer data)
{
    (void)object;
    (void)pspec;
    Manager *manager = data;
    if (manager->theme_mode == INFILTRATR_THEME_SYSTEM)
        manager_apply_theme(manager);
}

static void on_about(GtkButton *button, gpointer data)
{
    (void)button;
    Manager *manager = data;
    GtkWidget *dialog = gtk_about_dialog_new();
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(manager->window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_title(GTK_WINDOW(dialog), "About InfiltratorFS");
    gtk_about_dialog_set_program_name(
        GTK_ABOUT_DIALOG(dialog), manager_project_info.program_name);
    gtk_about_dialog_set_version(
        GTK_ABOUT_DIALOG(dialog), manager_project_info.version);
    gtk_about_dialog_set_comments(
        GTK_ABOUT_DIALOG(dialog), manager_project_info.comments);
    gtk_about_dialog_set_website(
        GTK_ABOUT_DIALOG(dialog), manager_project_info.website);
    gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(dialog), "Project website");
    gtk_about_dialog_set_copyright(
        GTK_ABOUT_DIALOG(dialog), manager_project_info.copyright_text);
    gtk_about_dialog_set_license(GTK_ABOUT_DIALOG(dialog),
        "GPL-3.0-or-later. See LICENSE in the source package for the complete licence text.");
    gtk_about_dialog_set_wrap_license(GTK_ABOUT_DIALOG(dialog), TRUE);
    const char *authors[] = { manager_project_info.author, NULL };
    gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), authors);
    add_class(dialog, "link-about-dialog");
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    if (theme) {
        GError *error = NULL;
        GdkPixbuf *logo = gtk_icon_theme_load_icon(theme, "drive-harddisk", 96,
                                                   GTK_ICON_LOOKUP_FORCE_SIZE, &error);
        if (logo) {
            gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(dialog), logo);
            g_object_unref(logo);
        }
        g_clear_error(&error);
    }
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static GtkWidget *action_row(Manager *manager, GtkWidget *parent,
                             const char *icon, const char *title,
                             const char *description, const char *button_text,
                             const char *semantic_style, GCallback callback)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    add_class(row, "action-row");
    if (semantic_style)
        add_class(row, semantic_style);
    gtk_box_pack_start(GTK_BOX(row), make_icon(icon, GTK_ICON_SIZE_BUTTON, 22),
                       FALSE, FALSE, 2);
    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(text), make_label(title, "action-title"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text), make_label(description, "action-description"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);
    GtkWidget *button = gtk_button_new_with_label(button_text);
    g_signal_connect(button, "clicked", callback, manager);
    gtk_box_pack_end(GTK_BOX(row), button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(parent), row, FALSE, FALSE, 0);
    return button;
}

static GtkWidget *stat_card(const char *caption, GtkWidget **value_out)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    add_class(card, "stat-card");
    gtk_box_pack_start(GTK_BOX(card), make_label(caption, "stat-caption"), FALSE, FALSE, 0);
    GtkWidget *value = make_label("—", "stat-value");
    gtk_label_set_ellipsize(GTK_LABEL(value), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(card), value, FALSE, FALSE, 0);
    *value_out = value;
    return card;
}

static void detail_row(GtkWidget *grid, int row, const char *caption,
                       GtkWidget **value_out)
{
    GtkWidget *caption_label = make_label(caption, "detail-caption");
    GtkWidget *value = make_label("—", "detail-value");
    gtk_label_set_selectable(GTK_LABEL(value), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(value), PANGO_ELLIPSIZE_MIDDLE);
    gtk_grid_attach(GTK_GRID(grid), caption_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), value, 1, row, 1, 1);
    *value_out = value;
}

static void clear_activity(GtkButton *button, gpointer data)
{
    (void)button;
    Manager *manager = data;
    gtk_text_buffer_set_text(manager->activity_buffer, "", -1);
}

static void refresh_button_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    manager_refresh_devices(data, TRUE);
}

static GtkWidget *build_ui(Manager *manager)
{
    manager->window = gtk_application_window_new(manager->app);
    gtk_window_set_title(GTK_WINDOW(manager->window), APP_NAME);
    gtk_window_set_default_size(GTK_WINDOW(manager->window), 1220, 780);
    gtk_widget_set_size_request(manager->window, 940, 620);

    GtkWidget *bar = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(bar), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(bar), APP_NAME);
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(bar), infilfs_manager_copy()->subtitle);
    gtk_window_set_titlebar(GTK_WINDOW(manager->window), bar);

    GtkWidget *new_image = gtk_button_new_with_label(infilfs_manager_copy()->new_image_button);
    g_signal_connect(new_image, "clicked", G_CALLBACK(on_create_image), manager);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(bar), new_image);
    GtkWidget *open_image = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(open_image), make_icon("document-open-symbolic", GTK_ICON_SIZE_BUTTON, 0));
    g_signal_connect(open_image, "clicked", G_CALLBACK(on_open_image), manager);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(bar), open_image);
    GtkWidget *refresh = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(refresh), make_icon("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON, 0));
    g_signal_connect(refresh, "clicked", G_CALLBACK(refresh_button_clicked), manager);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(bar), refresh);
    manager->theme_button = gtk_button_new_with_label("Theme: System");
    gtk_widget_set_tooltip_text(manager->theme_button,
        "Cycle Infiltratr Common System, Day and Night themes");
    g_signal_connect(manager->theme_button, "clicked", G_CALLBACK(on_theme), manager);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(bar), manager->theme_button);
    GtkWidget *about = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(about), make_icon("help-about-symbolic", GTK_ICON_SIZE_BUTTON, 0));
    g_signal_connect(about, "clicked", G_CALLBACK(on_about), manager);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(bar), about);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(manager->window), root);
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(paned), 286);
    gtk_box_pack_start(GTK_BOX(root), paned, TRUE, TRUE, 0);

    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(sidebar, "sidebar");
    GtkWidget *sidebar_title = make_label(infilfs_manager_copy()->storage_heading, "sidebar-title");
    gtk_widget_set_margin_start(sidebar_title, 18);
    gtk_widget_set_margin_top(sidebar_title, 18);
    gtk_widget_set_margin_bottom(sidebar_title, 8);
    gtk_box_pack_start(GTK_BOX(sidebar), sidebar_title, FALSE, FALSE, 0);
    manager->device_list = gtk_list_box_new();
    add_class(manager->device_list, "device-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(manager->device_list), GTK_SELECTION_SINGLE);
    g_signal_connect(manager->device_list, "row-selected", G_CALLBACK(on_device_selected), manager);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), manager->device_list);
    gtk_box_pack_start(GTK_BOX(sidebar), scroll, TRUE, TRUE, 0);
    manager->count_label = make_label("0 available partitions", "sidebar-count");
    gtk_widget_set_margin_start(manager->count_label, 18);
    gtk_widget_set_margin_bottom(manager->count_label, 12);
    gtk_box_pack_end(GTK_BOX(sidebar), manager->count_label, FALSE, FALSE, 0);
    gtk_paned_pack1(GTK_PANED(paned), sidebar, FALSE, FALSE);

    manager->content_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(manager->content_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_paned_pack2(GTK_PANED(paned), manager->content_stack, TRUE, FALSE);

    GtkWidget *empty_align = gtk_alignment_new(0.5f, 0.5f, 0.0f, 0.0f);
    GtkWidget *empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    add_class(empty, "empty-state");
    gtk_box_pack_start(GTK_BOX(empty), make_icon("drive-harddisk-symbolic", GTK_ICON_SIZE_DIALOG, 52), FALSE, FALSE, 0);
    GtkWidget *empty_title = make_label(infilfs_manager_copy()->empty_title, "empty-title");
    gtk_label_set_xalign(GTK_LABEL(empty_title), 0.5f);
    gtk_box_pack_start(GTK_BOX(empty), empty_title, FALSE, FALSE, 0);
    GtkWidget *empty_copy = make_label(infilfs_manager_copy()->empty_copy, "empty-copy");
    gtk_label_set_xalign(GTK_LABEL(empty_copy), 0.5f);
    gtk_box_pack_start(GTK_BOX(empty), empty_copy, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(empty_align), empty);
    gtk_stack_add_named(GTK_STACK(manager->content_stack), empty_align, "empty");

    GtkWidget *target_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    add_class(target_page, "content");
    GtkWidget *hero = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    manager->hero_icon = make_icon("drive-harddisk-symbolic", GTK_ICON_SIZE_DIALOG, 32);
    gtk_box_pack_start(GTK_BOX(hero), manager->hero_icon, FALSE, FALSE, 0);
    GtkWidget *identity = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    GtkWidget *title_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    manager->title_label = make_label("Volume", "hero-title");
    manager->mount_badge = make_label("Not mounted", "badge");
    gtk_box_pack_start(GTK_BOX(title_row), manager->title_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(title_row), manager->mount_badge, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(identity), title_row, FALSE, FALSE, 0);
    manager->path_label = make_label("", "hero-path");
    gtk_label_set_ellipsize(GTK_LABEL(manager->path_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(identity), manager->path_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hero), identity, TRUE, TRUE, 0);
    manager->unmount_button = gtk_button_new_with_label(infilfs_manager_copy()->unmount_button);
    g_signal_connect(manager->unmount_button, "clicked", G_CALLBACK(on_unmount), manager);
    gtk_box_pack_end(GTK_BOX(hero), manager->unmount_button, FALSE, FALSE, 0);
    manager->mount_button = gtk_button_new_with_label(infilfs_manager_copy()->mount_button);
    add_class(manager->mount_button, "suggested-action");
    g_signal_connect(manager->mount_button, "clicked", G_CALLBACK(on_mount), manager);
    gtk_box_pack_end(GTK_BOX(hero), manager->mount_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(target_page), hero, FALSE, FALSE, 0);

    manager->page_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(manager->page_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    GtkWidget *switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(manager->page_stack));
    gtk_widget_set_halign(switcher, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(target_page), switcher, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(target_page), manager->page_stack, TRUE, TRUE, 0);

    GtkWidget *overview_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(overview_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *overview = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_container_add(GTK_CONTAINER(overview_scroll), overview);
    gtk_stack_add_titled(GTK_STACK(manager->page_stack), overview_scroll, "overview", infilfs_manager_copy()->overview_tab);

    GtkWidget *stats = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(stats), 10);
    gtk_grid_set_column_homogeneous(GTK_GRID(stats), TRUE);
    GtkWidget *capacity_card = stat_card(infilfs_manager_copy()->capacity_caption, &manager->stat_size);
    GtkWidget *filesystem_card = stat_card(infilfs_manager_copy()->filesystem_caption, &manager->stat_fs);
    manager->stat_status_card = stat_card(infilfs_manager_copy()->status_caption, &manager->stat_mount);
    add_class(capacity_card, "stat-capacity");
    add_class(filesystem_card, "stat-filesystem");
    add_class(manager->stat_status_card, "stat-status");
    gtk_grid_attach(GTK_GRID(stats), capacity_card, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(stats), filesystem_card, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(stats), manager->stat_status_card, 2, 0, 1, 1);
    gtk_box_pack_start(GTK_BOX(overview), stats, FALSE, FALSE, 0);

    GtkWidget *details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 13);
    add_class(details, "card");
    add_class(details, "card-info");
    GtkWidget *details_title = make_label(infilfs_manager_copy()->volume_information, "section-title");
    add_class(details_title, "section-info");
    gtk_box_pack_start(GTK_BOX(details), details_title, FALSE, FALSE, 0);
    GtkWidget *detail_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(detail_grid), 30);
    gtk_grid_set_row_spacing(GTK_GRID(detail_grid), 10);
    detail_row(detail_grid, 0, infilfs_manager_copy()->device_type, &manager->value_type);
    detail_row(detail_grid, 1, infilfs_manager_copy()->device_path, &manager->value_path);
    detail_row(detail_grid, 2, "Filesystem", &manager->value_fs);
    detail_row(detail_grid, 3, infilfs_manager_copy()->volume_label, &manager->value_label);
    detail_row(detail_grid, 4, infilfs_manager_copy()->mount_state, &manager->value_mount);
    detail_row(detail_grid, 5, infilfs_manager_copy()->mount_point, &manager->value_mountpoint);
    gtk_box_pack_start(GTK_BOX(details), detail_grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(overview), details, FALSE, FALSE, 0);

    const struct infilfs_manager_copy *copy = infilfs_manager_copy();
    const struct infilfs_manager_action_descriptor *inspect =
        infilfs_manager_action(INFILFS_MANAGER_ACTION_INSPECT);
    const struct infilfs_manager_action_descriptor *check =
        infilfs_manager_action(INFILFS_MANAGER_ACTION_CHECK);
    const struct infilfs_manager_action_descriptor *scrub =
        infilfs_manager_action(INFILFS_MANAGER_ACTION_SCRUB);
    const struct infilfs_manager_action_descriptor *forensic =
        infilfs_manager_action(INFILFS_MANAGER_ACTION_FORENSIC);

    GtkWidget *maintenance = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    add_class(maintenance, "card");
    add_class(maintenance, "card-maintenance");
    GtkWidget *maintenance_title = make_label(copy->maintenance, "section-title");
    add_class(maintenance_title, "section-maintenance");
    gtk_box_pack_start(GTK_BOX(maintenance), maintenance_title, FALSE, FALSE, 0);
    manager->inspect_button = action_row(manager, maintenance, inspect->linux_icon,
        inspect->title, inspect->description, inspect->button,
        inspect->semantic_style, G_CALLBACK(on_inspect));
    manager->check_button = action_row(manager, maintenance, check->linux_icon,
        check->title, check->description, check->button,
        check->semantic_style, G_CALLBACK(on_check));
    manager->scrub_button = action_row(manager, maintenance, scrub->linux_icon,
        scrub->title, scrub->description, scrub->button,
        scrub->semantic_style, G_CALLBACK(on_scrub));
    manager->forensic_button = action_row(manager, maintenance, forensic->linux_icon,
        forensic->title, forensic->description, forensic->button,
        forensic->semantic_style, G_CALLBACK(on_forensic));
    gtk_box_pack_start(GTK_BOX(overview), maintenance, FALSE, FALSE, 0);

    GtkWidget *danger = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    add_class(danger, "danger-zone");
    GtkWidget *danger_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    GtkWidget *danger_title = make_label(infilfs_manager_copy()->danger_title, "section-title");
    add_class(danger_title, "section-danger");
    gtk_box_pack_start(GTK_BOX(danger_text), danger_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(danger_text), make_label(infilfs_manager_copy()->danger_description, "section-subtitle"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(danger), danger_text, TRUE, TRUE, 0);
    manager->format_button = gtk_button_new_with_label(infilfs_manager_copy()->format_button);
    add_class(manager->format_button, "destructive-action");
    g_signal_connect(manager->format_button, "clicked", G_CALLBACK(on_format), manager);
    gtk_box_pack_end(GTK_BOX(danger), manager->format_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(overview), danger, FALSE, FALSE, 0);

    GtkWidget *activity = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *activity_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(activity_top), make_label(infilfs_manager_copy()->activity_heading, "section-title"), TRUE, TRUE, 0);
    GtkWidget *clear = gtk_button_new_with_label(infilfs_manager_copy()->clear_button);
    g_signal_connect(clear, "clicked", G_CALLBACK(clear_activity), manager);
    gtk_box_pack_end(GTK_BOX(activity_top), clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(activity), activity_top, FALSE, FALSE, 0);
    GtkWidget *activity_frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(activity_frame, "activity-frame");
    GtkWidget *activity_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(activity_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    manager->activity_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(manager->activity_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(manager->activity_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(manager->activity_view), GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(manager->activity_view), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(manager->activity_view), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(manager->activity_view), 10);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(manager->activity_view), 10);
    add_class(manager->activity_view, "activity");
    manager->activity_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(manager->activity_view));
    gtk_container_add(GTK_CONTAINER(activity_scroll), manager->activity_view);
    gtk_box_pack_start(GTK_BOX(activity_frame), activity_scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(activity), activity_frame, TRUE, TRUE, 0);
    gtk_stack_add_titled(GTK_STACK(manager->page_stack), activity, "activity", "Activity");
    gtk_stack_add_named(GTK_STACK(manager->content_stack), target_page, "target");

    GtkWidget *statusbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    add_class(statusbar, "statusbar");
    manager->spinner = gtk_spinner_new();
    manager->status_label = make_label("Ready", "status-text");
    manager->version_label = make_label("Version " INFILFS_IMPLEMENTATION_VERSION, "status-text");
    gtk_label_set_xalign(GTK_LABEL(manager->version_label), 1.0f);
    gtk_box_pack_start(GTK_BOX(statusbar), manager->spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(statusbar), manager->status_label, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(statusbar), manager->version_label, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(root), statusbar, FALSE, FALSE, 0);

    GtkSettings *settings = gtk_settings_get_default();
    if (settings) {
        g_signal_connect(settings, "notify::gtk-application-prefer-dark-theme",
                         G_CALLBACK(on_system_theme_changed), manager);
        g_signal_connect(settings, "notify::gtk-theme-name",
                         G_CALLBACK(on_system_theme_changed), manager);
    }
    manager_apply_theme(manager);
    return manager->window;
}

static gboolean select_requested_device_idle(gpointer data)
{
    Manager *manager = data;
    if (!manager->format_device)
        return G_SOURCE_REMOVE;
    char *requested = canonical_path(manager->format_device);
    GList *children = gtk_container_get_children(GTK_CONTAINER(manager->device_list));
    GtkListBoxRow *match = NULL;
    for (GList *node = children; node; node = node->next) {
        Target *target = g_object_get_data(G_OBJECT(node->data), "infiltratorfs-target");
        char *candidate = target ? canonical_path(target->path) : NULL;
        if (candidate && strcmp(candidate, requested) == 0)
            match = GTK_LIST_BOX_ROW(node->data);
        g_free(candidate);
        if (match)
            break;
    }
    g_list_free(children);
    g_free(requested);
    if (!match) {
        char *message = g_strdup_printf(
            "Refusing to format %s: it is not an available non-system partition.",
            manager->format_device);
        manager_error(manager, message);
        g_free(message);
        return G_SOURCE_REMOVE;
    }
    gtk_list_box_select_row(GTK_LIST_BOX(manager->device_list), match);
    manager_start_format(manager);
    return G_SOURCE_REMOVE;
}

static gboolean startup_check_idle(gpointer data)
{
    Manager *manager = data;
    puts("infiltratorfs-manager: GTK startup PASS");
    g_application_quit(G_APPLICATION(manager->app));
    return G_SOURCE_REMOVE;
}

static void manager_destroy(gpointer data)
{
    Manager *manager = data;
    if (!manager)
        return;
    if (manager->targets)
        g_ptr_array_free(manager->targets, TRUE);
    target_free(manager->image_target);
    if (manager->css)
        g_object_unref(manager->css);
    g_free(manager->format_device);
    g_free(manager);
}

static void app_activate(GtkApplication *app, gpointer data)
{
    Manager *manager = data;
    if (manager->window) {
        gtk_window_present(GTK_WINDOW(manager->window));
        return;
    }
    manager->app = app;
    manager->theme_mode = load_theme_mode();
    build_ui(manager);
    manager_refresh_devices(manager, FALSE);
    manager_show_target(manager);
    gtk_widget_show_all(manager->window);
    gtk_widget_hide(manager->spinner);
    if (manager->format_device)
        g_idle_add(select_requested_device_idle, manager);
    if (manager->startup_check)
        g_idle_add(startup_check_idle, manager);
}

static int list_partitions(void)
{
    char *error_text = NULL;
    GPtrArray *targets = discover_partitions(&error_text);
    if (!targets) {
        fprintf(stderr, "%s\n", error_text ? error_text : "Could not enumerate storage devices.");
        g_free(error_text);
        return 1;
    }
    for (guint i = 0; i < targets->len; ++i) {
        Target *target = g_ptr_array_index(targets, i);
        printf("%s\t%" PRIu64 "\t%s\t%s\t%s\t%s\n",
               target->path, target->size, target->media,
               (target->filesystem && *target->filesystem) ? target->filesystem : "-",
               (target->label && *target->label) ? target->label : "-",
               (target->mountpoint && *target->mountpoint) ? target->mountpoint : "-");
    }
    g_ptr_array_free(targets, TRUE);
    return 0;
}

static gboolean required_command(const char *name)
{
    char *path = g_find_program_in_path(name);
    if (path) {
        g_free(path);
        return TRUE;
    }
    fprintf(stderr, "%s: missing required command: %s\n", APP_NAME, name);
    return FALSE;
}

int main(int argc, char **argv)
{
    gboolean list = FALSE;
    gboolean startup_check = FALSE;
    char *format_device = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--list-partitions") == 0) {
            list = TRUE;
        } else if (strcmp(argv[i], "--startup-check") == 0) {
            startup_check = TRUE;
        } else if (strcmp(argv[i], "--format-device") == 0 && i + 1 < argc) {
            format_device = g_strdup(argv[++i]);
        } else {
            fprintf(stderr,
                "Usage: %s [--list-partitions] [--format-device PATH] [--startup-check]\n",
                argv[0]);
            g_free(format_device);
            return 2;
        }
    }
    if (list) {
        int result = list_partitions();
        g_free(format_device);
        return result;
    }

    static const char *required[] = {
        "lsblk", "findmnt", "mountpoint", "pkexec", "xdg-open", "truncate", NULL
    };
    for (size_t i = 0; required[i]; ++i) {
        if (!required_command(required[i])) {
            g_free(format_device);
            return 1;
        }
    }

    Manager *manager = g_new0(Manager, 1);
    if (!manager) {
        g_free(format_device);
        return 1;
    }
    manager->startup_check = startup_check;
    manager->format_device = format_device;
    GtkApplication *app = gtk_application_new(
        APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(app_activate), manager);
    int result = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    manager_destroy(manager);
    return result;
}
