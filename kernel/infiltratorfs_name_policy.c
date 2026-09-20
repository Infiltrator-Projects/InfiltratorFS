// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"

bool infilfs_removable_name_valid_v1(const unsigned char *name, size_t length)
{
    static const char * const reserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    char base[5] = { 0 };
    size_t base_length = 0;
    size_t i;

    if (!name || !length || length > 255u ||
        name[length - 1u] == '.' || name[length - 1u] == ' ')
        return false;

    for (i = 0; i < length; ++i) {
        unsigned char c = name[i];
        if (c < 0x20u || c == 0x7fu ||
            c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
            return false;
        if (c == '.')
            break;
        if (base_length < sizeof(base) - 1u) {
            if (c >= 'a' && c <= 'z')
                c = (unsigned char)(c - ('a' - 'A'));
            base[base_length++] = (char)c;
        } else {
            base_length = sizeof(base);
        }
    }

    if (base_length < sizeof(base)) {
        for (i = 0; i < ARRAY_SIZE(reserved); ++i)
            if (strlen(reserved[i]) == base_length &&
                !memcmp(base, reserved[i], base_length))
                return false;
    }
    return true;
}

int infilfs_name_validate(struct super_block *sb, const struct qstr *name)
{
    struct infilfs_sb_info *sbi = sb ? INFILFS_SB(sb) : NULL;
    bool reserved_linux_meta = name &&
        name->len == sizeof(INFILFS_LINUX_META_DIRECTORY) - 1u &&
        !memcmp(name->name, INFILFS_LINUX_META_DIRECTORY, name->len);

    if (!name || !name->len || name->len > INFILFS_NAME_MAX ||
        !infilfs_rw_utf8_valid(name->name, name->len) ||
        (name->len == 1 && name->name[0] == '.') ||
        (name->len == 2 && name->name[0] == '.' && name->name[1] == '.') ||
        reserved_linux_meta ||
        (sbi &&
         (le64_to_cpu(sbi->disk.incompat_flags) &
          INFILFS_INCOMPAT_REMOVABLE_NAMES_V1) &&
         !infilfs_removable_name_valid_v1(name->name, name->len)))
        return -EINVAL;
    return 0;
}
