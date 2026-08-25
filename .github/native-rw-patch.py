from pathlib import Path


def replace_once(text, old, new, label):
    if new in text:
        return text
    if old not in text:
        raise SystemExit(f"{label} marker not found")
    return text.replace(old, new, 1)


# Fix the one compatibility issue in the separately added RW engine.
p = Path("kernel/infiltratorfs_rw.inc")
s = p.read_text()
s = replace_once(
    s,
    """    grown = kvrealloc(all, bytes + rec, GFP_NOFS);\n    if (!grown) {\n        ret = -ENOMEM;\n        goto out;\n    }\n    all = grown;\n""",
    """    grown = kvmalloc(bytes + rec, GFP_NOFS);\n    if (!grown) {\n        ret = -ENOMEM;\n        goto out;\n    }\n    if (bytes)\n        memcpy(grown, all, bytes);\n    kvfree(all);\n    all = grown;\n""",
    "RW buffer growth",
)
p.write_text(s)

# Add the tiny snapshot-catalog payload layout used to reject unsafe native
# writable mounts when retained snapshots exist.
p = Path("kernel/infiltratorfs_format.h")
s = p.read_text()
marker = """struct infilfs_index_entry_disk {\n    __u8 object_id[16];\n    __le64 object_block;\n    __le16 object_type;\n    __le16 flags;\n    __le32 reserved;\n} __packed;\n"""
addition = marker + """\nstruct infilfs_snapshot_catalog_payload_disk {\n    __le32 snapshot_count;\n    __le32 reserved;\n} __packed;\n"""
if "struct infilfs_snapshot_catalog_payload_disk" not in s:
    if marker not in s:
        raise SystemExit("format index-entry marker not found")
    s = s.replace(marker, addition, 1)
assert_marker = "static_assert(sizeof(struct infilfs_index_entry_disk) == 32);\n"
assert_new = assert_marker + "static_assert(sizeof(struct infilfs_snapshot_catalog_payload_disk) == 8);\n"
s = replace_once(s, assert_marker, assert_new, "format snapshot static assert")
p.write_text(s)

# Wire the RW engine into the VFS adapter.
p = Path("kernel/infiltratorfs.c")
s = p.read_text()
include_marker = "#include <linux/fs_context.h>\n"
includes = """#include <linux/fs_context.h>\n#include <linux/cred.h>\n#include <linux/kernel.h>\n#include <linux/mutex.h>\n#include <linux/random.h>\n#include <linux/timekeeping.h>\n#include <linux/uidgid.h>\n#include <linux/user_namespace.h>\n#include <linux/vmalloc.h>\n"""
if "#include <linux/mutex.h>" not in s:
    s = replace_once(s, include_marker, includes, "kernel include")

old_sbi = """struct infilfs_sb_info {\n    struct infilfs_superblock_disk disk;\n    u64 device_blocks;\n};\n"""
new_sbi = """struct infilfs_sb_info {\n    struct infilfs_superblock_disk disk;\n    u64 device_blocks;\n    struct mutex write_lock;\n    u8 *bitmap;\n    size_t bitmap_bytes;\n    bool rw_enabled;\n    bool write_poisoned;\n};\n"""
if "struct mutex write_lock;" not in s:
    s = replace_once(s, old_sbi, new_sbi, "sbi")

decl = "static const struct inode_operations infilfs_symlink_inode_operations;\n"
file_decl = decl + "static const struct inode_operations infilfs_file_inode_operations;\n"
s = replace_once(s, decl, file_decl, "file inode ops declaration")

# Empty paged directories are valid and are required for freshly created dirs.
old_empty = """        if (page_count == 0 || page_count > INFILFS_DIRECTORY_PAGE_POINTERS) {\n            ret = -EFSCORRUPTED;\n            goto out;\n        }\n"""
new_empty = """        if (page_count == 0) {\n            ret = le32_to_cpu(payload->entry_count) == 0 ? 0 : -EFSCORRUPTED;\n            goto out;\n        }\n        if (page_count > INFILFS_DIRECTORY_PAGE_POINTERS) {\n            ret = -EFSCORRUPTED;\n            goto out;\n        }\n"""
if old_empty in s:
    s = s.replace(old_empty, new_empty, 1)

file_pop = """        inode->i_mode = S_IFREG | (permissions ? permissions : 0444);\n        inode->i_fop = &infilfs_file_operations;\n"""
file_pop_new = """        inode->i_mode = S_IFREG | (permissions ? permissions : 0444);\n        inode->i_op = &infilfs_file_inode_operations;\n        inode->i_fop = &infilfs_file_operations;\n"""
s = replace_once(s, file_pop, file_pop_new, "file inode population")

owner_old = """    inode->i_uid = GLOBAL_ROOT_UID;\n    inode->i_gid = GLOBAL_ROOT_GID;\n    ret = 0;\n"""
owner_new = """    {\n        kuid_t uid = make_kuid(&init_user_ns, le32_to_cpu(posix->uid));\n        kgid_t gid = make_kgid(&init_user_ns, le32_to_cpu(posix->gid));\n        inode->i_uid = uid_valid(uid) ? uid : GLOBAL_ROOT_UID;\n        inode->i_gid = gid_valid(gid) ? gid : GLOBAL_ROOT_GID;\n    }\n    ret = 0;\n"""
if owner_old in s:
    s = s.replace(owner_old, owner_new, 1)

insert_marker = "static void infilfs_evict_inode(struct inode *inode)\n"
if '#include "infiltratorfs_rw.inc"' not in s:
    s = replace_once(
        s,
        insert_marker,
        '#include "infiltratorfs_rw.inc"\n\n' + insert_marker,
        "RW include insertion",
    )

put_old = """static void infilfs_put_super(struct super_block *sb)\n{\n    kfree(sb->s_fs_info);\n    sb->s_fs_info = NULL;\n}\n"""
put_new = """static void infilfs_put_super(struct super_block *sb)\n{\n    infilfs_rw_mount_destroy(sb);\n    kfree(sb->s_fs_info);\n    sb->s_fs_info = NULL;\n}\n"""
if put_old in s:
    s = s.replace(put_old, put_new, 1)

dir_ops_old = """static const struct inode_operations infilfs_dir_inode_operations = {\n    .lookup = infilfs_lookup,\n};\n"""
dir_ops_new = """static const struct inode_operations infilfs_dir_inode_operations = {\n    .lookup = infilfs_lookup,\n    .create = infilfs_rw_create,\n    .mkdir = infilfs_rw_mkdir,\n};\n\nstatic const struct inode_operations infilfs_file_inode_operations = {\n    .setattr = infilfs_rw_setattr,\n};\n"""
if dir_ops_old in s:
    s = s.replace(dir_ops_old, dir_ops_new, 1)

fops_old = """static const struct file_operations infilfs_file_operations = {\n    .owner = THIS_MODULE,\n    .llseek = generic_file_llseek,\n    .read_iter = infilfs_file_read_iter,\n};\n"""
fops_new = """static const struct file_operations infilfs_file_operations = {\n    .owner = THIS_MODULE,\n    .llseek = generic_file_llseek,\n    .read_iter = infilfs_file_read_iter,\n    .write_iter = infilfs_file_write_iter,\n    .fsync = infilfs_file_fsync,\n};\n"""
if fops_old in s:
    s = s.replace(fops_old, fops_new, 1)

ro_reject = """    if (!(sb->s_flags & SB_RDONLY))\n        return -EROFS;\n"""
if ro_reject in s:
    s = s.replace(ro_reject, "", 1)

sbi_alloc = """    sbi->device_blocks = bytes >> INFILFS_DISK_BLOCK_SHIFT;\n    sb->s_fs_info = sbi;\n\n    ret = infilfs_select_checkpoint(sb, &sbi->disk);\n"""
sbi_alloc_new = """    sbi->device_blocks = bytes >> INFILFS_DISK_BLOCK_SHIFT;\n    mutex_init(&sbi->write_lock);\n    sb->s_fs_info = sbi;\n\n    ret = infilfs_select_checkpoint(sb, &sbi->disk);\n"""
if sbi_alloc in s:
    s = s.replace(sbi_alloc, sbi_alloc_new, 1)

select_marker = """    ret = infilfs_select_checkpoint(sb, &sbi->disk);\n    if (ret)\n        goto fail;\n\n    sb->s_magic = INFILTRATORFS_MAGIC;\n"""
select_new = """    ret = infilfs_select_checkpoint(sb, &sbi->disk);\n    if (ret)\n        goto fail;\n    ret = infilfs_rw_mount_init(sb);\n    if (ret)\n        goto fail;\n\n    sb->s_magic = INFILTRATORFS_MAGIC;\n"""
if select_marker in s:
    s = s.replace(select_marker, select_new, 1)

log_old = """    pr_info(\"InfiltratorFS: native read-only mount Format %u.%u generation %llu\\n\",\n            INFILFS_FORMAT_MAJOR, INFILFS_FORMAT_MINOR,\n            (unsigned long long)le64_to_cpu(sbi->disk.generation));\n"""
log_new = """    pr_info(\"InfiltratorFS: native %s mount Format %u.%u generation %llu\\n\",\n            sb_rdonly(sb) ? \"read-only\" : \"read-write\",\n            INFILFS_FORMAT_MAJOR, INFILFS_FORMAT_MINOR,\n            (unsigned long long)le64_to_cpu(sbi->disk.generation));\n"""
if log_old in s:
    s = s.replace(log_old, log_new, 1)

fail_old = """fail:\n    kfree(sbi);\n    sb->s_fs_info = NULL;\n    return ret;\n}\n\nstatic int infilfs_get_tree(struct fs_context *fc)\n{\n    /* The native adapter is intentionally read-only. Reject writable mounts\n     * before get_tree_bdev() attempts to open the block device for writing. */\n    if (!(fc->sb_flags & SB_RDONLY))\n        return -EROFS;\n    return get_tree_bdev(fc, infilfs_fill_super);\n}\n"""
fail_new = """fail:\n    infilfs_rw_mount_destroy(sb);\n    kfree(sbi);\n    sb->s_fs_info = NULL;\n    return ret;\n}\n\nstatic int infilfs_get_tree(struct fs_context *fc)\n{\n    return get_tree_bdev(fc, infilfs_fill_super);\n}\n"""
if fail_old in s:
    s = s.replace(fail_old, fail_new, 1)

s = s.replace(
    'pr_info("InfiltratorFS: native Linux read-only VFS registered\\n");',
    'pr_info("InfiltratorFS: native Linux VFS registered with initial RW support\\n");',
)
s = s.replace(
    'MODULE_DESCRIPTION("InfiltratorFS native Linux read-only VFS driver");',
    'MODULE_DESCRIPTION("InfiltratorFS native Linux VFS driver with initial RW support");',
)
p.write_text(s)

# The .run installer is unmanaged by dpkg. Purge an installed older .deb before
# replacing its files so dpkg-query cannot continue claiming an obsolete build.
p = Path("support/installer/bootstrap.sh")
s = p.read_text()
marker = "install_kernel_module() {\n"
helper = """remove_conflicting_debian_package() {\n    local status version\n    command -v dpkg-query >/dev/null 2>&1 || return 0\n    status=\"$(dpkg-query -W -f='${db:Status-Abbrev} ${Version}\\n' infiltratorfs 2>/dev/null || true)\"\n    [[ \"$status\" == ii\\ * ]] || return 0\n    version=\"${status#ii }\"\n    printf 'Removing Debian package record for InfiltratorFS %s before native .run installation.\\n' \"$version\"\n    printf 'The .run installation is native/unmanaged; use the .deb when dpkg ownership is desired.\\n'\n    run_as_root dpkg --purge infiltratorfs\n}\n\n"""
if "remove_conflicting_debian_package()" not in s:
    if marker not in s:
        raise SystemExit("bootstrap kernel marker not found")
    s = s.replace(marker, helper + marker, 1)
copy_marker = '    run_as_root install -m 0644 "$ROOT/kernel/infiltratorfs_format.h" "$DKMS_SOURCE/infiltratorfs_format.h"\n'
if '"$ROOT/kernel/infiltratorfs_rw.inc"' not in s:
    if copy_marker not in s:
        raise SystemExit("bootstrap kernel source-copy marker not found")
    s = s.replace(
        copy_marker,
        copy_marker + '    run_as_root install -m 0644 "$ROOT/kernel/infiltratorfs_rw.inc" "$DKMS_SOURCE/infiltratorfs_rw.inc"\n',
        1,
    )
dry = "    printf 'Older InfiltratorFS DKMS registrations would be removed before any package/header installation.\\n'\n"
if "An installed InfiltratorFS .deb would be purged" not in s:
    if dry not in s:
        raise SystemExit("bootstrap dry-run marker not found")
    s = s.replace(
        dry,
        dry + "    printf 'An installed InfiltratorFS .deb would be purged before this unmanaged native install.\\n'\n",
        1,
    )
call = "remove_stale_dkms_versions\n\nprintf 'InfiltratorFS native build installer\\n'\n"
if "remove_conflicting_debian_package\n" not in s:
    if call not in s:
        raise SystemExit("bootstrap normal-run cleanup marker not found")
    s = s.replace(
        call,
        "remove_stale_dkms_versions\nremove_conflicting_debian_package\n\nprintf 'InfiltratorFS native build installer\\n'\n",
        1,
    )
p.write_text(s)

# Ship the extra source include in both DKMS packaging paths.
p = Path("packaging/build-linux-packages.sh")
s = p.read_text()
copy = 'install -m 0644 kernel/infiltratorfs_format.h "$dkms_root/infiltratorfs_format.h"\n'
if "kernel/infiltratorfs_rw.inc" not in s:
    if copy not in s:
        raise SystemExit("package kernel source-copy marker not found")
    s = s.replace(
        copy,
        copy + 'install -m 0644 kernel/infiltratorfs_rw.inc "$dkms_root/infiltratorfs_rw.inc"\n',
        1,
    )
validate = 'grep -q "usr/src/infiltratorfs-${version}/infiltratorfs_format.h$" "$dist_dir/package-contents.txt"\n'
if "infiltratorfs_rw.inc$" not in s:
    if validate not in s:
        raise SystemExit("package validation marker not found")
    s = s.replace(
        validate,
        validate + 'grep -q "usr/src/infiltratorfs-${version}/infiltratorfs_rw.inc$" "$dist_dir/package-contents.txt"\n',
        1,
    )
s = s.replace(
    "native read-only Linux kernel module via DKMS",
    "native Linux kernel module via DKMS with initial read-write support",
)
p.write_text(s)

# New implementation milestone; Format 0.12 is unchanged.
p = Path("CMakeLists.txt")
s = p.read_text()
old = "project(InfiltratorFS VERSION 0.16.14 LANGUAGES C)"
new = "project(InfiltratorFS VERSION 0.17.0 LANGUAGES C)"
if old not in s and new not in s:
    raise SystemExit("CMake version marker not found")
p.write_text(s.replace(old, new, 1))
