#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path


def replace(path, old, new, count=1):
    p = Path(path)
    text = p.read_text()
    actual = text.count(old)
    if actual != count:
        raise SystemExit(
            f"{path}: expected {count} occurrence(s), found {actual}: {old[:100]!r}"
        )
    p.write_text(text.replace(old, new))


replace(
    "CMakeLists.txt",
    "project(InfiltratorFS VERSION 0.18.42 LANGUAGES C)\n# Development qualification anchor: 0.18.42",
    "project(InfiltratorFS VERSION 0.18.43 LANGUAGES C)\n# Development qualification anchor: 0.18.43",
)
replace(
    "README.md",
    "**Current source version:** 0.18.42 (Format 0.18)<br>",
    "**Current source version:** 0.18.43 (Format 0.18)<br>",
)
replace("README.md", "**Current source:** 0.18.42  ", "**Current source:** 0.18.43  ")
replace("README.md", "**Latest published release:** v0.18.41  ", "**Latest published release:** v0.18.42  ")

replace(
    "docs/COMPRESSION.md",
    """The compressed representation is selected only when it saves at least
one-sixteenth (6.25 percent) of the logical filesystem blocks in the bounded
cluster. Clusters up to 16 blocks retain the historical one-block minimum.
Otherwise the data is stored normally, avoiding decode cost for marginal wins.
""",
    """The compressed representation is selected whenever it consumes at least one
fewer 4096-byte filesystem block than the uncompressed representation. Otherwise
the data is stored normally. There is deliberately no additional percentage
threshold: a real block saved is useful space saved.
""",
)
replace(
    "docs/COMPRESSION.md",
    "ones. Portable-core writes use the same IAC1 format and savings rules. Persistent",
    "ones. Portable-core writes use the same IAC1 format and physical-block rule. Persistent",
)

replace(
    "include/infilfs/iac1.h",
    """#define INFS_IAC1_MIN_SAVINGS_DIVISOR 16u

/* Require at least 1/16 of the logical filesystem blocks to be saved.
 * For clusters up to 16 blocks this is the historical one-block minimum;
 * larger clusters must earn proportionally more space before paying the
 * ongoing decode cost. This is a write policy only, never stream metadata. */
static inline int infs_iac1_savings_worthwhile(
    infs_iac1_size logical_blocks, infs_iac1_size stored_blocks)
{
    infs_iac1_size required;

    if (!logical_blocks || !stored_blocks || stored_blocks >= logical_blocks)
        return 0;
    required = logical_blocks / INFS_IAC1_MIN_SAVINGS_DIVISOR +
        ((logical_blocks % INFS_IAC1_MIN_SAVINGS_DIVISOR) != 0);
    return logical_blocks - stored_blocks >= required;
}

""",
    "",
)
replace(
    "kernel/infiltratorfs_rw_data.inc",
    """            if (!infs_iac1_savings_worthwhile(
                    logical_blocks, stored_blocks))
                stored_blocks = 0;
""",
    """            if (stored_blocks >= logical_blocks)
                stored_blocks = 0;
""",
)
replace(
    "src/volume/compression.inc",
    """            if (!infs_iac1_savings_worthwhile(
                    logical_blocks, stored_blocks))
                stored_blocks = 0;
""",
    """            if (stored_blocks >= logical_blocks)
                stored_blocks = 0;
""",
)
replace(
    "tests/compression-codec.c",
    """    expect(infs_iac1_savings_worthwhile(16u, 15u),
           \"one-block saving accepted for 16-block cluster\");
    expect(!infs_iac1_savings_worthwhile(64u, 61u),
           \"shallow saving rejected for 64-block cluster\");
    expect(infs_iac1_savings_worthwhile(64u, 60u),
           \"one-sixteenth saving accepted for 64-block cluster\");
""",
    "",
)

replace(
    "kernel/infiltratorfs_internal.h",
    "#include <linux/pagemap.h>\n#include <linux/random.h>",
    "#include <linux/pagemap.h>\n#include <linux/posix_acl.h>\n#include <linux/posix_acl_xattr.h>\n#include <linux/random.h>",
)

replace(
    "kernel/infiltratorfs_rw.inc",
    "#define infilfs_rw_setattr infilfs_rw_setattr_legacy\n#define infilfs_rw_mount_destroy infilfs_rw_mount_destroy_legacy",
    "#define infilfs_rw_setattr infilfs_rw_setattr_legacy\n#define infilfs_rw_mount_init infilfs_rw_mount_init_legacy\n#define infilfs_rw_mount_destroy infilfs_rw_mount_destroy_legacy",
)
replace(
    "kernel/infiltratorfs_rw.inc",
    "#undef infilfs_rw_setattr\n#undef infilfs_rw_mount_destroy",
    """#undef infilfs_rw_setattr
#undef infilfs_rw_mount_init
#undef infilfs_rw_mount_destroy

static int infilfs_rw_mount_init(struct super_block *sb)
{
    int ret = infilfs_rw_mount_init_legacy(sb);

    if (!ret)
        sb->s_flags |= SB_POSIXACL;
    return ret;
}""",
)

acl_code = r'''

/*
 * Native Linux POSIX ACL adapter.
 *
 * Format 0.18 deliberately keeps Linux ACLs out of the portable object
 * layout. Store the Linux POSIX ACL xattr representation in the existing
 * hidden Linux metadata sidecar. The VFS owns permission evaluation,
 * idmapped-mount translation and ACL caching; this adapter supplies durable
 * get/set plus create/chmod integration.
 */
static const char *infilfs_posix_acl_suffix(int type)
{
    switch (type) {
    case ACL_TYPE_ACCESS:
        return "posix_acl_access";
    case ACL_TYPE_DEFAULT:
        return "posix_acl_default";
    default:
        return NULL;
    }
}

static struct posix_acl *infilfs_posix_acl_get(
    struct inode *inode, int type, bool rcu)
{
    const char *suffix = infilfs_posix_acl_suffix(type);
    void *value = NULL;
    struct posix_acl *acl;
    int size;
    int got;
    int ret;

    if (rcu)
        return ERR_PTR(-ECHILD);
    if (!suffix)
        return ERR_PTR(-EINVAL);
    if (type == ACL_TYPE_DEFAULT && !S_ISDIR(inode->i_mode))
        return NULL;

    size = infilfs_linux_xattr_get(
        &infilfs_linux_system_xattr_handler, NULL, inode,
        suffix, NULL, 0);
    if (size == -ENODATA)
        return NULL;
    if (size < 0)
        return ERR_PTR(size);
    if (!size)
        return ERR_PTR(-EFSCORRUPTED);

    value = kvmalloc((size_t)size, GFP_NOFS);
    if (!value)
        return ERR_PTR(-ENOMEM);
    got = infilfs_linux_xattr_get(
        &infilfs_linux_system_xattr_handler, NULL, inode,
        suffix, value, (size_t)size);
    if (got != size) {
        kvfree(value);
        return ERR_PTR(got < 0 ? got : -EIO);
    }

    acl = posix_acl_from_xattr(&init_user_ns, value, (size_t)size);
    kvfree(value);
    if (IS_ERR_OR_NULL(acl))
        return acl ? acl : ERR_PTR(-EFSCORRUPTED);
    ret = posix_acl_valid(&init_user_ns, acl);
    if (ret) {
        posix_acl_release(acl);
        return ERR_PTR(ret == -ENOMEM ? ret : -EFSCORRUPTED);
    }
    return acl;
}

static int infilfs_posix_acl_encode(
    const struct posix_acl *acl, void **value_out, size_t *size_out)
{
    void *value;
    size_t size;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
    value = posix_acl_to_xattr(&init_user_ns, acl, &size, GFP_NOFS);
    if (!value)
        return -ENOMEM;
#else
    int encoded;

    encoded = posix_acl_to_xattr(&init_user_ns, acl, NULL, 0);
    if (encoded < 0)
        return encoded;
    size = (size_t)encoded;
    value = kvmalloc(size, GFP_NOFS);
    if (!value)
        return -ENOMEM;
    encoded = posix_acl_to_xattr(&init_user_ns, acl, value, size);
    if (encoded < 0 || (size_t)encoded != size) {
        kvfree(value);
        return encoded < 0 ? encoded : -EIO;
    }
#endif
    *value_out = value;
    *size_out = size;
    return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_acl_store(
    struct mnt_idmap *idmap, struct dentry *dentry,
#else
static int infilfs_posix_acl_store(
    struct user_namespace *idmap, struct dentry *dentry,
#endif
    struct inode *inode, int type, const struct posix_acl *acl)
{
    const char *suffix = infilfs_posix_acl_suffix(type);
    void *value = NULL;
    size_t size = 0;
    int ret;

    if (!suffix)
        return -EINVAL;
    if (!acl) {
        ret = infilfs_linux_xattr_set(
            &infilfs_linux_system_xattr_handler, idmap, dentry,
            inode, suffix, NULL, 0, 0);
        return ret == -ENODATA ? 0 : ret;
    }
    ret = infilfs_posix_acl_encode(acl, &value, &size);
    if (ret)
        return ret;
    ret = infilfs_linux_xattr_set(
        &infilfs_linux_system_xattr_handler, idmap, dentry,
        inode, suffix, value, size, 0);
    kvfree(value);
    return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_acl_set(
    struct mnt_idmap *idmap, struct dentry *dentry,
#else
static int infilfs_posix_acl_set(
    struct user_namespace *idmap, struct dentry *dentry,
#endif
    struct posix_acl *acl, int type)
{
    struct inode *inode = d_inode(dentry);
    struct posix_acl *old_acl;
    struct posix_acl *stored_acl = acl;
    umode_t old_mode = inode->i_mode;
    umode_t mode = old_mode;
    struct iattr attr;
    int rollback_ret;
    int ret;

    if (type != ACL_TYPE_ACCESS && type != ACL_TYPE_DEFAULT)
        return -EINVAL;
    if (type == ACL_TYPE_DEFAULT && !S_ISDIR(inode->i_mode)) {
        if (acl)
            return -EACCES;
        set_cached_acl(inode, type, NULL);
        return 0;
    }
    if (acl) {
        ret = posix_acl_valid(&init_user_ns, acl);
        if (ret)
            return ret;
    }
    if (type == ACL_TYPE_ACCESS && stored_acl) {
        ret = posix_acl_update_mode(idmap, inode, &mode, &stored_acl);
        if (ret)
            return ret;
    }

    old_acl = infilfs_posix_acl_get(inode, type, false);
    if (IS_ERR(old_acl))
        return PTR_ERR(old_acl);

    ret = infilfs_posix_acl_store(idmap, dentry, inode, type, stored_acl);
    if (ret)
        goto out_old;

    if (type == ACL_TYPE_ACCESS && mode != old_mode) {
        memset(&attr, 0, sizeof(attr));
        attr.ia_valid = ATTR_MODE | ATTR_CTIME;
        attr.ia_mode = mode;
        attr.ia_ctime = current_time(inode);
        ret = infilfs_posix_rewrite_inode(inode, &attr);
        if (ret) {
            rollback_ret = infilfs_posix_acl_store(
                idmap, dentry, inode, type, old_acl);
            if (rollback_ret) {
                INFILFS_SB(inode->i_sb)->write_poisoned = true;
                pr_err("InfiltratorFS: ACL rollback failed for inode %lu: %d\n",
                       inode->i_ino, rollback_ret);
            }
            goto out_old;
        }
        inode->i_mode = (inode->i_mode & S_IFMT) | (mode & 07777);
    }
    set_cached_acl(inode, type, stored_acl);
    ret = 0;
out_old:
    posix_acl_release(old_acl);
    return ret;
}

static int infilfs_posix_acl_prepare_create(
    struct inode *dir, umode_t *mode, umode_t type,
    struct posix_acl **default_acl, struct posix_acl **access_acl)
{
    umode_t acl_mode = (*mode & 07777) | type;
    int ret;

    ret = posix_acl_create(dir, &acl_mode, default_acl, access_acl);
    if (!ret)
        *mode = (*mode & S_IFMT) | (acl_mode & 07777);
    return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_acl_finish_create(
    struct mnt_idmap *idmap, struct inode *dir,
#else
static int infilfs_posix_acl_finish_create(
    struct user_namespace *idmap, struct inode *dir,
#endif
    struct dentry *dentry, struct posix_acl *default_acl,
    struct posix_acl *access_acl, bool is_directory)
{
    struct inode *inode = d_inode(dentry);
    int cleanup;
    int ret = 0;

    if (!inode)
        return -EIO;
    if (access_acl)
        ret = infilfs_posix_acl_store(
            idmap, dentry, inode, ACL_TYPE_ACCESS, access_acl);
    if (!ret && default_acl)
        ret = infilfs_posix_acl_store(
            idmap, dentry, inode, ACL_TYPE_DEFAULT, default_acl);
    if (!ret) {
        set_cached_acl(inode, ACL_TYPE_ACCESS, access_acl);
        if (is_directory)
            set_cached_acl(inode, ACL_TYPE_DEFAULT, default_acl);
        return 0;
    }

    (void)infilfs_linux_meta_remove_object(
        inode->i_sb, INFILFS_I(inode)->object_id);
    cleanup = is_directory ?
        infilfs_ns_rmdir(dir, dentry) : infilfs_ns_unlink(dir, dentry);
    if (!cleanup)
        d_drop(dentry);
    else
        pr_err("InfiltratorFS: could not remove object after ACL create failure: %d\n",
               cleanup);
    return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_acl_create(
    struct mnt_idmap *idmap, struct inode *dir,
#else
static int infilfs_posix_acl_create(
    struct user_namespace *idmap, struct inode *dir,
#endif
    struct dentry *dentry, umode_t mode, bool excl)
{
    struct posix_acl *default_acl = NULL;
    struct posix_acl *access_acl = NULL;
    int ret;

    ret = infilfs_posix_acl_prepare_create(
        dir, &mode, S_IFREG, &default_acl, &access_acl);
    if (!ret)
        ret = infilfs_posix_create(idmap, dir, dentry, mode, excl);
    if (!ret)
        ret = infilfs_posix_acl_finish_create(
            idmap, dir, dentry, default_acl, access_acl, false);
    posix_acl_release(access_acl);
    posix_acl_release(default_acl);
    return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
static struct dentry *infilfs_posix_acl_mkdir(
    struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, umode_t mode)
{
    struct posix_acl *default_acl = NULL;
    struct posix_acl *access_acl = NULL;
    struct dentry *result;
    int ret;

    ret = infilfs_posix_acl_prepare_create(
        dir, &mode, S_IFDIR, &default_acl, &access_acl);
    if (ret)
        return ERR_PTR(ret);
    result = infilfs_posix_mkdir(idmap, dir, dentry, mode);
    if (IS_ERR(result)) {
        posix_acl_release(access_acl);
        posix_acl_release(default_acl);
        return result;
    }
    ret = infilfs_posix_acl_finish_create(
        idmap, dir, dentry, default_acl, access_acl, true);
    posix_acl_release(access_acl);
    posix_acl_release(default_acl);
    return ret ? ERR_PTR(ret) : result;
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_acl_mkdir(
    struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, umode_t mode)
#else
static int infilfs_posix_acl_mkdir(
    struct user_namespace *idmap, struct inode *dir,
    struct dentry *dentry, umode_t mode)
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 17, 0)
{
    struct posix_acl *default_acl = NULL;
    struct posix_acl *access_acl = NULL;
    int ret;

    ret = infilfs_posix_acl_prepare_create(
        dir, &mode, S_IFDIR, &default_acl, &access_acl);
    if (!ret)
        ret = infilfs_posix_mkdir(idmap, dir, dentry, mode);
    if (!ret)
        ret = infilfs_posix_acl_finish_create(
            idmap, dir, dentry, default_acl, access_acl, true);
    posix_acl_release(access_acl);
    posix_acl_release(default_acl);
    return ret;
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_acl_mknod(
    struct mnt_idmap *idmap, struct inode *dir,
#else
static int infilfs_posix_acl_mknod(
    struct user_namespace *idmap, struct inode *dir,
#endif
    struct dentry *dentry, umode_t mode, dev_t rdev)
{
    struct posix_acl *default_acl = NULL;
    struct posix_acl *access_acl = NULL;
    umode_t type = mode & S_IFMT;
    int ret;

    if (!type)
        type = S_IFREG;
    ret = infilfs_posix_acl_prepare_create(
        dir, &mode, type, &default_acl, &access_acl);
    if (!ret)
        ret = infilfs_posix_mknod(idmap, dir, dentry, mode, rdev);
    if (!ret)
        ret = infilfs_posix_acl_finish_create(
            idmap, dir, dentry, default_acl, access_acl, false);
    posix_acl_release(access_acl);
    posix_acl_release(default_acl);
    return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_acl_setattr(
    struct mnt_idmap *idmap, struct dentry *dentry,
#else
static int infilfs_posix_acl_setattr(
    struct user_namespace *idmap, struct dentry *dentry,
#endif
    struct iattr *attr)
{
    bool mode_changed = (attr->ia_valid & ATTR_MODE) != 0;
    int ret = infilfs_posix_setattr(idmap, dentry, attr);

    if (!ret && mode_changed)
        ret = posix_acl_chmod(idmap, dentry, d_inode(dentry)->i_mode);
    return ret;
}
'''

p = Path("kernel/infiltratorfs_linux_meta.inc")
text = p.read_text()
if "static struct posix_acl *infilfs_posix_acl_get(" in text:
    raise SystemExit("POSIX ACL adapter already present")
if not text.endswith("}\n"):
    raise SystemExit("unexpected Linux metadata file ending")
p.write_text(text + acl_code)

replace(
    "kernel/infiltratorfs_rw.inc",
    """#define infilfs_rw_create infilfs_posix_create, \\
    .mknod = infilfs_posix_mknod, \\
    .listxattr = infilfs_linux_listxattr
#define infilfs_rw_mkdir infilfs_posix_mkdir, \\
    .unlink = infilfs_ns_unlink, \\
    .rmdir = infilfs_ns_rmdir, \\
    .link = infilfs_ns_link, \\
    .symlink = infilfs_posix_symlink, \\
    .rename = infilfs_ns_rename, \\
    .setattr = infilfs_posix_setattr, \\
    .getattr = infilfs_getattr
#define infilfs_rw_setattr infilfs_posix_setattr, \\
    .getattr = infilfs_getattr, \\
    .listxattr = infilfs_linux_listxattr
""",
    """#define infilfs_rw_create infilfs_posix_acl_create, \\
    .mknod = infilfs_posix_acl_mknod, \\
    .get_inode_acl = infilfs_posix_acl_get, \\
    .set_acl = infilfs_posix_acl_set, \\
    .listxattr = infilfs_linux_listxattr
#define infilfs_rw_mkdir infilfs_posix_acl_mkdir, \\
    .unlink = infilfs_ns_unlink, \\
    .rmdir = infilfs_ns_rmdir, \\
    .link = infilfs_ns_link, \\
    .symlink = infilfs_posix_symlink, \\
    .rename = infilfs_ns_rename, \\
    .setattr = infilfs_posix_acl_setattr, \\
    .getattr = infilfs_getattr, \\
    .get_inode_acl = infilfs_posix_acl_get, \\
    .set_acl = infilfs_posix_acl_set
#define infilfs_rw_setattr infilfs_posix_acl_setattr, \\
    .getattr = infilfs_getattr, \\
    .get_inode_acl = infilfs_posix_acl_get, \\
    .set_acl = infilfs_posix_acl_set, \\
    .listxattr = infilfs_linux_listxattr
""",
)

replace(
    ".github/workflows/kernel-module.yml",
    "sudo apt-get install -y build-essential cmake e2fsprogs kmod linux-headers-generic",
    "sudo apt-get install -y acl build-essential cmake e2fsprogs kmod linux-headers-generic rsync",
)

acl_test_anchor = """          python3 - \"$mountpoint/user-owned/file.txt\" <<'PY'\n          import os\n          import sys\n          assert 'trusted.infiltratorfs-ci' not in os.listxattr(sys.argv[1])\n          PY\n\n          sudo mkdir \"$mountpoint/native-rw\"\n"""
acl_test_block = """          python3 - \"$mountpoint/user-owned/file.txt\" <<'PY'\n          import os\n          import sys\n          assert 'trusted.infiltratorfs-ci' not in os.listxattr(sys.argv[1])\n          PY\n\n          # POSIX ACLs are a root-filesystem requirement, not merely arbitrary\n          # system.* xattrs. Prove VFS enforcement, chmod mask updates, default\n          # inheritance and rsync -aA preservation on the mounted native path.\n          acl_root=\"$mountpoint/posix-acl-ci\"\n          mkdir \"$acl_root\"\n          printf 'acl-readable\\n' > \"$acl_root/access.txt\"\n          chmod 0600 \"$acl_root/access.txt\"\n          if sudo -u nobody cat \"$acl_root/access.txt\" >/dev/null 2>&1; then\n            echo 'nobody unexpectedly read mode-0600 ACL test file' >&2\n            exit 1\n          fi\n          setfacl -m u:nobody:r-- \"$acl_root/access.txt\"\n          getfacl -cp \"$acl_root/access.txt\" | grep -Eq '^user:nobody:r--$'\n          test \"$(sudo -u nobody cat \"$acl_root/access.txt\")\" = 'acl-readable'\n          chmod 0600 \"$acl_root/access.txt\"\n          getfacl -cp \"$acl_root/access.txt\" | grep -Eq '^mask::---$'\n          if sudo -u nobody cat \"$acl_root/access.txt\" >/dev/null 2>&1; then\n            echo 'chmod did not restrict the POSIX ACL mask' >&2\n            exit 1\n          fi\n          chmod 0640 \"$acl_root/access.txt\"\n          getfacl -cp \"$acl_root/access.txt\" | grep -Eq '^mask::r--$'\n          test \"$(sudo -u nobody cat \"$acl_root/access.txt\")\" = 'acl-readable'\n\n          mkdir \"$acl_root/inherit\"\n          chmod 0755 \"$acl_root/inherit\"\n          setfacl -m d:u:nobody:r--,d:m::r-x \"$acl_root/inherit\"\n          ( umask 077; printf 'inherited\\n' > \"$acl_root/inherit/child.txt\" )\n          getfacl -cp \"$acl_root/inherit/child.txt\" | grep -Eq '^user:nobody:r--$'\n          test \"$(sudo -u nobody cat \"$acl_root/inherit/child.txt\")\" = 'inherited'\n\n          mkdir \"$acl_root/rsync-src\" \"$acl_root/rsync-dst\"\n          printf 'rsync-acl\\n' > \"$acl_root/rsync-src/file.txt\"\n          setfacl -m u:nobody:r-- \"$acl_root/rsync-src/file.txt\"\n          rsync -aA \"$acl_root/rsync-src/\" \"$acl_root/rsync-dst/\"\n          getfacl -cp \"$acl_root/rsync-src/file.txt\" > \"$RUNNER_TEMP/acl-src.txt\"\n          getfacl -cp \"$acl_root/rsync-dst/file.txt\" > \"$RUNNER_TEMP/acl-dst.txt\"\n          diff -u \"$RUNNER_TEMP/acl-src.txt\" \"$RUNNER_TEMP/acl-dst.txt\"\n          sync\n\n          sudo mkdir \"$mountpoint/native-rw\"\n"""
replace(".github/workflows/kernel-module.yml", acl_test_anchor, acl_test_block)

policy = Path("tests/native-posix-acl-policy.sh")
if policy.exists():
    raise SystemExit("native POSIX ACL policy guard already exists")
policy.write_text(
    """#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root="${1:-.}"
meta="$root/kernel/infiltratorfs_linux_meta.inc"
rw="$root/kernel/infiltratorfs_rw.inc"
internal="$root/kernel/infiltratorfs_internal.h"
grep -Fq 'posix_acl_from_xattr' "$meta"
grep -Fq 'posix_acl_to_xattr' "$meta"
grep -Fq 'posix_acl_create(' "$meta"
grep -Fq 'posix_acl_chmod(' "$meta"
grep -Fq 'posix_acl_update_mode' "$meta"
grep -Fq '.get_inode_acl = infilfs_posix_acl_get' "$rw"
grep -Fq '.set_acl = infilfs_posix_acl_set' "$rw"
grep -Fq 'sb->s_flags |= SB_POSIXACL' "$rw"
grep -Fq '#include <linux/posix_acl.h>' "$internal"
grep -Fq '#include <linux/posix_acl_xattr.h>' "$internal"
! grep -Fq 'INFS_IAC1_MIN_SAVINGS_DIVISOR' "$root/include/infilfs/iac1.h"
! grep -R -Fq 'infs_iac1_savings_worthwhile' \
    "$root/kernel" "$root/src" "$root/tests" "$root/include"
"""
)
policy.chmod(0o755)

replace(
    ".github/workflows/kernel-module.yml",
    "      - name: Guard native quotas\n        run: bash tests/native-quota-policy.sh .\n",
    "      - name: Guard native quotas\n        run: bash tests/native-quota-policy.sh .\n\n      - name: Guard native POSIX ACLs\n        run: bash tests/native-posix-acl-policy.sh .\n",
)

print("0.18.43 patch applied cleanly")
