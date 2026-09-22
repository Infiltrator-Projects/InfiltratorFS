// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"

static u8 infilfs_casefold_byte_v1(u8 value)
{
    return value >= 'A' && value <= 'Z' ?
        (u8)(value + ('a' - 'A')) : value;
}

bool infilfs_casefold_names_enabled(const struct super_block *sb)
{
    const struct infilfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;

    return sbi &&
        (le64_to_cpu(sbi->disk.incompat_flags) &
         INFILFS_INCOMPAT_CASEFOLD_V1) != 0;
}

bool infilfs_name_equal(const struct super_block *sb,
                        const u8 *left, size_t left_length,
                        const u8 *right, size_t right_length)
{
    size_t i;

    if (!left || !right || left_length != right_length)
        return false;
    if (!infilfs_casefold_names_enabled(sb))
        return !memcmp(left, right, left_length);
    for (i = 0; i < left_length; ++i)
        if (infilfs_casefold_byte_v1(left[i]) !=
            infilfs_casefold_byte_v1(right[i]))
            return false;
    return true;
}

u32 infilfs_name_hash(const struct super_block *sb,
                      const u8 *name, size_t length)
{
    u32 hash = 2166136261u;
    size_t i;

    for (i = 0; i < length; ++i) {
        u8 value = infilfs_casefold_names_enabled(sb) ?
            infilfs_casefold_byte_v1(name[i]) : name[i];
        hash ^= value;
        hash *= 16777619u;
    }
    return hash;
}

static int infilfs_casefold_d_hash(const struct dentry *dentry,
                                   struct qstr *name)
{
    unsigned long hash = init_name_hash(dentry);
    unsigned int i;

    for (i = 0; i < name->len; ++i)
        hash = partial_name_hash(
            infilfs_casefold_byte_v1(name->name[i]), hash);
    name->hash = end_name_hash(hash);
    return 0;
}

static int infilfs_casefold_d_compare(const struct dentry *dentry,
                                      unsigned int len, const char *str,
                                      const struct qstr *name)
{
    unsigned int i;

    (void)dentry;
    if (len != name->len)
        return 1;
    for (i = 0; i < len; ++i)
        if (infilfs_casefold_byte_v1((u8)str[i]) !=
            infilfs_casefold_byte_v1(name->name[i]))
            return 1;
    return 0;
}

const struct dentry_operations infilfs_casefold_dentry_ops = {
    .d_hash = infilfs_casefold_d_hash,
    .d_compare = infilfs_casefold_d_compare,
};

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

bool infilfs_name_is_reserved_linux_meta(const struct qstr *name)
{
    return name &&
        name->len == sizeof(INFILFS_LINUX_META_DIRECTORY) - 1u &&
        !memcmp(name->name, INFILFS_LINUX_META_DIRECTORY, name->len);
}

int infilfs_name_validate(struct super_block *sb, const struct qstr *name)
{
    struct infilfs_sb_info *sbi = sb ? INFILFS_SB(sb) : NULL;
    bool reserved_linux_meta = infilfs_name_is_reserved_linux_meta(name) ||
        (name && infilfs_casefold_names_enabled(sb) &&
         infilfs_name_equal(
             sb, name->name, name->len,
             (const u8 *)INFILFS_LINUX_META_DIRECTORY,
             sizeof(INFILFS_LINUX_META_DIRECTORY) - 1u));

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
