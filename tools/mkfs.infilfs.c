// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/format.h"
#include "infilfs/format_volume.h"
#include "infilfs/locator.h"
#include "infilfs/fs.h"
#include "infilfs/posix_io.h"
#include "infilfs/utf8.h"

#include "infilfs/endian.h"
#include "infiltratr/arithmetic.h"
#include "infiltratr/core.h"
#include "infiltratr/posix.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define INFS_MIN_SIZE_BYTES (16ull * 1024ull * 1024ull)

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [--force] [--wipe-only] [--removable-profile] [--casefold] [-L label] <image-or-block-device>\n", prog);
    fprintf(stderr, "       %s --experimental-019-image --force [-L label] <regular-image>\n", prog);
}

static int parse_identity_component(const char *text, uint32_t *value)
{
    uint64_t parsed = 0;
    if (!value ||
        !infiltratr_parse_u64_range(text, 10, 0, UINT32_MAX, &parsed))
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

/*
 * mkfs is commonly elevated with sudo or pkexec because a block device needs
 * raw write access.  The filesystem root should belong to the desktop user who
 * requested the format, not to uid 0 merely because the formatter had to be
 * privileged.  Direct root invocation remains root-owned.
 */
static void resolve_root_owner(uint32_t *uid_out, uint32_t *gid_out)
{
    uint32_t uid = (uint32_t)getuid();
    uint32_t gid = (uint32_t)getgid();

    if (geteuid() == 0) {
        const char *sudo_uid = getenv("SUDO_UID");
        const char *sudo_gid = getenv("SUDO_GID");
        const char *pkexec_uid = getenv("PKEXEC_UID");
        uint32_t elevated_uid;

        if (parse_identity_component(sudo_uid, &elevated_uid) == 0) {
            uid = elevated_uid;
            if (parse_identity_component(sudo_gid, &gid) != 0) {
                struct passwd *pw = getpwuid((uid_t)uid);
                if (pw)
                    gid = (uint32_t)pw->pw_gid;
            }
        } else if (parse_identity_component(pkexec_uid, &elevated_uid) == 0) {
            struct passwd *pw;
            uid = elevated_uid;
            pw = getpwuid((uid_t)uid);
            if (pw)
                gid = (uint32_t)pw->pw_gid;
        }
    }

    *uid_out = uid;
    *gid_out = gid;
}

static int same_or_descendant_path(const char *parent, const char *candidate)
{
    size_t length = strlen(parent);
    if (!infiltratr_string_starts_with(candidate, parent))
        return 0;
    return candidate[length] == '\0' || candidate[length] == '/';
}

static int directory_has_entries(const char *path)
{
    DIR *directory = opendir(path);
    if (!directory)
        return -1;
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0) {
            result = 1;
            break;
        }
    }
    closedir(directory);
    return result;
}

/* Return 1 when the target or any child partition is mounted/held by another
 * block layer, 0 when it is demonstrably unused, and -1 when safety cannot be
 * established. Formatting a real block target fails closed on -1. */
static int block_device_in_use(int fd)
{
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISBLK(st.st_mode))
        return -1;

    char target_link[PATH_MAX];
    char target_real[PATH_MAX];
    snprintf(target_link, sizeof(target_link), "/sys/dev/block/%u:%u",
             major(st.st_rdev), minor(st.st_rdev));
    if (!infiltratr_realpath_copy(target_link, target_real,
                                  sizeof(target_real)))
        return -1;

    FILE *mountinfo = fopen("/proc/self/mountinfo", "r");
    if (!mountinfo)
        return -1;
    char line[8192];
    while (fgets(line, sizeof(line), mountinfo)) {
        unsigned mount_major = 0, mount_minor = 0;
        if (sscanf(line, "%*u %*u %u:%u", &mount_major, &mount_minor) != 2)
            continue;
        char mount_link[PATH_MAX];
        char mount_real[PATH_MAX];
        snprintf(mount_link, sizeof(mount_link), "/sys/dev/block/%u:%u",
                 mount_major, mount_minor);
        if (infiltratr_realpath_copy(mount_link, mount_real,
                                     sizeof(mount_real)) &&
            same_or_descendant_path(target_real, mount_real)) {
            fclose(mountinfo);
            return 1;
        }
    }
    if (ferror(mountinfo)) {
        fclose(mountinfo);
        return -1;
    }
    fclose(mountinfo);

    DIR *classes = opendir("/sys/class/block");
    if (!classes)
        return -1;
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(classes)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        char class_path[PATH_MAX];
        char class_real[PATH_MAX];
        if (snprintf(class_path, sizeof(class_path), "/sys/class/block/%s",
                     entry->d_name) >= (int)sizeof(class_path) ||
            !infiltratr_realpath_copy(class_path, class_real,
                                      sizeof(class_real)))
            continue;
        if (!same_or_descendant_path(target_real, class_real))
            continue;
        char holders[PATH_MAX];
        if (snprintf(holders, sizeof(holders), "%s/holders", class_path) >=
            (int)sizeof(holders)) {
            result = -1;
            break;
        }
        int held = directory_has_entries(holders);
        if (held < 0) {
            result = -1;
            break;
        }
        if (held > 0) {
            result = 1;
            break;
        }
    }
    closedir(classes);
    return result;
}

int main(int argc, char **argv)
{
    int force = 0;
    int wipe_only = 0;
    int removable_profile = 0;
    int casefold = 0;
    int experimental019 = 0;
    const char *label = "InfiltratorFS";
    const char *path = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--force") == 0) {
            force = 1;
        } else if (strcmp(argv[i], "--wipe-only") == 0) {
            wipe_only = 1;
        } else if (strcmp(argv[i], "--removable-profile") == 0) {
            removable_profile = 1;
        } else if (strcmp(argv[i], "--casefold") == 0) {
            casefold = 1;
        } else if (strcmp(argv[i], "--experimental-019-image") == 0) {
            experimental019 = 1;
        } else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            label = argv[++i];
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else if (!path) {
            path = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!path) {
        usage(argv[0]);
        return 2;
    }
    if (strlen(label) >= INFS_LABEL_MAX) {
        fprintf(stderr, "Label is too long (maximum %u bytes).\n",
                INFS_LABEL_MAX - 1u);
        return 2;
    }
    if (!infs_utf8_validate(label, strlen(label))) {
        fprintf(stderr, "Label must be valid UTF-8.\n");
        return 2;
    }

    if (experimental019) {
        /* Physical Format 0.19 is not qualified. This explicit experimental
         * path formats ONLY an existing regular file through the guarded
         * portable format-0.19 writer. No block-device force override. */
        if (!force || wipe_only || removable_profile || casefold) {
            fprintf(stderr, "Format 0.19 image requires --force and "
                    "rejects wipe/profile/casefold options.\n");
            return 2;
        }
        struct infs_storage image = {0};
        infs_status status = infs_posix_storage_open(&image, path, 1);
        if (status != INFS_STATUS_OK) {
            fprintf(stderr, "Experimental 0.19 image open: %s\n",
                    infs_status_string(status));
            return 1;
        }
        uint64_t image_size = 0;
        int is_device = 0;
        status = infs_storage_get_size(&image, &image_size, &is_device);
        if (status == INFS_STATUS_OK && is_device)
            status = INFS_STATUS_NOT_SUPPORTED;
        if (status == INFS_STATUS_OK)
            status = infs_format_storage_019_test_image(&image, label);
        infs_storage_close(&image);
        if (status != INFS_STATUS_OK) {
            fprintf(stderr, "Experimental 0.19 image format refused: %s\n",
                    infs_status_string(status));
            return 1;
        }
        printf("Experimental InfiltratorFS Format 0.19 image created "
               "(%" PRIu64 " bytes; NOT qualified for physical devices).\n",
               image_size);
        return 0;
    }

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    uint64_t size_bytes = 0;
    int is_block = 0;
    if (infs_get_size_bytes(fd, &size_bytes, &is_block) != 0) {
        perror("size");
        close(fd);
        return 1;
    }

    if (is_block && !force) {
        fprintf(stderr, "Refusing to format a block device without --force.\n");
        close(fd);
        return 1;
    }
    if (is_block) {
        struct stat original;
        if (fstat(fd, &original) != 0 || !S_ISBLK(original.st_mode)) {
            fprintf(stderr,
                    "Refusing to format because the block target identity changed.\n");
            close(fd);
            return 1;
        }

        int in_use = block_device_in_use(fd);
        if (in_use > 0) {
            fprintf(stderr,
                    "Refusing to format an active block device or a disk with an active child partition.\n");
            close(fd);
            return 1;
        }
        if (in_use < 0) {
            fprintf(stderr,
                    "Refusing to format because block-device usage could not be verified safely.\n");
            close(fd);
            return 1;
        }

        /* The sysfs/mount checks are advisory preflight. Re-open the exact
         * target with Linux block-device exclusivity before the first write so
         * a target that became busy during preflight still fails closed. */
        close(fd);
        fd = open(path, O_RDWR | O_CLOEXEC | O_EXCL);
        if (fd < 0) {
            perror("exclusive block-device open");
            fprintf(stderr,
                    "Refusing to format because the block device could not be held exclusively.\n");
            return 1;
        }

        struct stat locked;
        if (fstat(fd, &locked) != 0 || !S_ISBLK(locked.st_mode) ||
            locked.st_rdev != original.st_rdev) {
            fprintf(stderr,
                    "Refusing to format because the block target changed during preflight.\n");
            close(fd);
            return 1;
        }
        uint64_t locked_size = 0;
        int locked_is_block = 0;
        if (infs_get_size_bytes(fd, &locked_size, &locked_is_block) != 0 ||
            !locked_is_block || locked_size != size_bytes) {
            fprintf(stderr,
                    "Refusing to format because the block target geometry changed during preflight.\n");
            close(fd);
            return 1;
        }
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        perror("exclusive storage lock");
        fprintf(stderr,
                "Refusing to format storage that is open by another InfiltratorFS process.\n");
        close(fd);
        return 1;
    }
    if (size_bytes < INFS_MIN_SIZE_BYTES) {
        fprintf(stderr, "Target is too small; minimum is 16 MiB.\n");
        close(fd);
        return 1;
    }

    struct infs_storage storage = {0};
    infs_status status = infs_posix_storage_adopt_fd(&storage, fd);
    if (status != INFS_STATUS_OK) {
        fprintf(stderr, "Storage setup: %s\n", infs_status_string(status));
        close(fd);
        return 1;
    }
    if (wipe_only) {
        struct infs_superblock_disk candidates[INFS_LOCATOR_MAX_CANDIDATES];
        uint8_t valid[INFS_LOCATOR_MAX_CANDIDATES];
        unsigned found = 0;
        status = infs_locator_discover_019_headers(&storage, size_bytes,
                                                   candidates, valid, &found);
        if (status != INFS_STATUS_OK) {
            infs_storage_close(&storage);
            fputs("Target is not an identifiable InfiltratorFS volume.\n", stderr);
            return 3;
        }
        uint8_t zero[INFS_BLOCK_SIZE] = {0};
        for (unsigned i = 0; status == INFS_STATUS_OK &&
                             i < INFS_LOCATOR_MAX_CANDIDATES; ++i) {
            if (!valid[i])
                continue;
            for (unsigned c = 0; status == INFS_STATUS_OK &&
                                 c < INFS_CHECKPOINT_COUNT; ++c)
                status = infs_storage_write(&storage,
                    infs_le64_to_cpu(candidates[i].checkpoint_block[c]) *
                        INFS_BLOCK_SIZE, zero, sizeof(zero));
        }
        for (unsigned i = 0; status == INFS_STATUS_OK && i < 2u; ++i)
            status = infs_storage_write(&storage,
                (i ? INFS_LOCATOR_BLOCK_B : INFS_LOCATOR_BLOCK_A) *
                    INFS_BLOCK_SIZE, zero, sizeof(zero));
        if (status == INFS_STATUS_OK)
            status = infs_storage_flush(&storage);
        infs_storage_close(&storage);
        if (status != INFS_STATUS_OK) {
            fprintf(stderr, "Signature wipe: %s\n", infs_status_string(status));
            return 1;
        }
        printf("Cleared InfiltratorFS signatures on %s.\n", path);
        return 0;
    }
    uint32_t root_uid = 0, root_gid = 0;
    resolve_root_owner(&root_uid, &root_gid);
    uint32_t options =
        (removable_profile ? INFS_FORMAT_OPTION_REMOVABLE_NAMES_V1 : 0u) |
        (casefold ? INFS_FORMAT_OPTION_CASEFOLD_V1 : 0u);
    status = infs_format_storage_with_root_owner(&storage, label, options,
                                                 root_uid, root_gid);
    uint8_t raw[INFS_BLOCK_SIZE];
    struct infs_superblock_disk sb;
    if (status == INFS_STATUS_OK)
        status = infs_storage_read(&storage, 0, raw, sizeof(raw));
    if (status == INFS_STATUS_OK)
        status = infs_decode_superblock(raw, &sb);
    infs_storage_close(&storage);
    if (status != INFS_STATUS_OK) {
        fprintf(stderr, "Format: %s\n", infs_status_string(status));
        return 1;
    }
    char uuid[37];
    infs_uuid_to_string(sb.filesystem_uuid, uuid);
    puts("InfiltratorFS formatted successfully");
    printf("  Implementation: %s\n", INFILFS_IMPLEMENTATION_VERSION);
    printf("  Format:          %u.%u\n", INFS_FORMAT_MAJOR, INFS_FORMAT_MINOR);
    printf("  UUID:            %s\n", uuid);
    printf("  Label:           %s\n", label);
    printf("  Root owner:      %" PRIu32 ":%" PRIu32 "\n", root_uid, root_gid);
    printf("  Block size:      %u bytes\n", INFS_BLOCK_SIZE);
    printf("  Total blocks:    %" PRIu64 "\n", infs_le64_to_cpu(sb.total_blocks));
    printf("  Free blocks:     %" PRIu64 "\n", infs_le64_to_cpu(sb.free_blocks));
    printf("  Allocation root: block %" PRIu64 "\n",
           infs_le64_to_cpu(sb.allocation_root_block));
    printf("  Allocation leaves: %" PRIu64 "\n",
           infs_le64_to_cpu(sb.allocation_leaf_count));
    printf("  Object index:    block %" PRIu64 "\n",
           infs_le64_to_cpu(sb.object_index_block));
    printf("  Root block:      block %" PRIu64 "\n",
           infs_le64_to_cpu(sb.root_object_block));
    printf("  Checkpoints:     %" PRIu64 ", %" PRIu64 ", %" PRIu64 "\n",
           infs_le64_to_cpu(sb.checkpoint_block[0]),
           infs_le64_to_cpu(sb.checkpoint_block[1]),
           infs_le64_to_cpu(sb.checkpoint_block[2]));
    return 0;
}
