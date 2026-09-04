#!/usr/bin/env python3
from pathlib import Path

root = Path('.')
kernel = root / 'kernel'
rw_path = kernel / 'infiltratorfs_rw.inc'
core_path = kernel / 'infiltratorfs_core.c'
internal_path = kernel / 'infiltratorfs_internal.h'
make_path = kernel / 'Makefile'
policy_path = root / 'tests/native-kernel-maintainability-policy.sh'
old_meta = kernel / 'infiltratorfs_linux_meta.inc'
new_meta = kernel / 'infiltratorfs_linux_meta.c'


def replace_once(path, old, new):
    data = path.read_text()
    if old not in data:
        raise SystemExit(f'missing expected text in {path}: {old!r}')
    path.write_text(data.replace(old, new, 1))

if not old_meta.exists() or new_meta.exists():
    raise SystemExit('Linux metadata source state is not the expected pre-extraction tree')
meta = old_meta.read_text()
meta = '#include "infiltratorfs_internal.h"\n' + meta
# The size ceiling is shared with quota because quota policy itself is stored in
# this Linux sidecar xattr representation. Keep a single private-driver value.
meta = meta.replace('#define INFILFS_LINUX_META_MAX (1024u * 1024u)\n', '', 1)
for old, new in (
    ('static int infilfs_linux_meta_get_special(', 'int infilfs_linux_meta_get_special('),
    ('static bool infilfs_linux_meta_directory_is_internal(', 'bool infilfs_linux_meta_directory_is_internal('),
    ('static int infilfs_linux_meta_remove_object(', 'int infilfs_linux_meta_remove_object('),
    ('static int infilfs_linux_xattr_get(', 'int infilfs_linux_xattr_get('),
    ('static int infilfs_linux_xattr_set(', 'int infilfs_linux_xattr_set('),
    ('static ssize_t infilfs_linux_listxattr(', 'ssize_t infilfs_linux_listxattr('),
    ('static const struct xattr_handler infilfs_linux_trusted_xattr_handler = {',
     'const struct xattr_handler infilfs_linux_trusted_xattr_handler = {'),
    ('static const struct xattr_handler * const infilfs_xattr_handlers[] = {',
     'const struct xattr_handler * const infilfs_xattr_handlers[] = {'),
    ('static int infilfs_posix_mknod(', 'int infilfs_posix_mknod('),
):
    if old not in meta:
        raise SystemExit(f'missing Linux metadata entry point: {old}')
    # xattr_set and mknod each have two version-selected declarations; promote both.
    meta = meta.replace(old, new)
new_meta.write_text(meta)
old_meta.unlink()

replace_once(rw_path, '#include "infiltratorfs_linux_meta.inc"\n', '')

core = core_path.read_text()
proto = '''static int infilfs_linux_meta_get_special(struct super_block *sb,\n                                          const u8 object_id[16],\n                                          umode_t *mode, dev_t *rdev);\nstatic bool infilfs_linux_meta_directory_is_internal(struct super_block *sb);\nstatic int infilfs_linux_meta_remove_object(struct super_block *sb,\n                                            const u8 object_id[16]);\n\n'''
if proto not in core:
    raise SystemExit('core Linux metadata prototype block not found')
core = core.replace(proto, '', 1)
core = core.replace('static const struct inode_operations infilfs_file_inode_operations;',
                    'const struct inode_operations infilfs_file_inode_operations;', 1)
core = core.replace('static const struct inode_operations infilfs_file_inode_operations = {',
                    'const struct inode_operations infilfs_file_inode_operations = {', 1)
core_path.write_text(core)

internal = internal_path.read_text()
marker = '\n#endif /* INFILTRATORFS_INTERNAL_H */\n'
api = '''\n/* Services owned by the compiled Linux sidecar-metadata adapter. */\n#define INFILFS_LINUX_META_MAX (1024u * 1024u)\nint infilfs_linux_meta_get_special(struct super_block *sb,\n                                   const u8 object_id[16],\n                                   umode_t *mode, dev_t *rdev);\nbool infilfs_linux_meta_directory_is_internal(struct super_block *sb);\nint infilfs_linux_meta_remove_object(struct super_block *sb,\n                                     const u8 object_id[16]);\nint infilfs_linux_xattr_get(\n    const struct xattr_handler *handler, struct dentry *dentry,\n    struct inode *inode, const char *name, void *buffer, size_t size);\n#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)\nint infilfs_linux_xattr_set(\n    const struct xattr_handler *handler, struct mnt_idmap *idmap,\n    struct dentry *dentry, struct inode *inode, const char *name,\n    const void *value, size_t value_size, int flags);\n#else\nint infilfs_linux_xattr_set(\n    const struct xattr_handler *handler, struct user_namespace *idmap,\n    struct dentry *dentry, struct inode *inode, const char *name,\n    const void *value, size_t value_size, int flags);\n#endif\nextern const struct xattr_handler infilfs_linux_trusted_xattr_handler;\nssize_t infilfs_linux_listxattr(struct dentry *dentry, char *list, size_t size);\nextern const struct xattr_handler * const infilfs_xattr_handlers[];\n#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)\nint infilfs_posix_mknod(struct mnt_idmap *idmap, struct inode *dir,\n                        struct dentry *dentry, umode_t mode, dev_t rdev);\n#else\nint infilfs_posix_mknod(struct user_namespace *idmap, struct inode *dir,\n                        struct dentry *dentry, umode_t mode, dev_t rdev);\n#endif\nextern const struct inode_operations infilfs_file_inode_operations;\n'''
if marker not in internal:
    raise SystemExit('internal header terminator not found')
internal_path.write_text(internal.replace(marker, api + marker, 1))

make = make_path.read_text()
old_line = 'infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o infiltratorfs_resize.o infiltratorfs_index_tree.o infiltratorfs_parallel_alloc.o infiltratorfs_allocation_publish.o infiltratorfs_read_cache.o infiltratorfs_pagecache.o infiltratorfs_directory_tree.o'
new_line = old_line + ' infiltratorfs_linux_meta.o'
if old_line not in make:
    raise SystemExit('Kbuild object list not found')
make = make.replace(old_line, new_line, 1)
make = make.replace('#   infiltratorfs_directory_tree.c owns scalable Format 0.17 directory trees.\n',
                    '#   infiltratorfs_directory_tree.c owns scalable Format 0.17 directory trees.\n#   infiltratorfs_linux_meta.c owns Linux-only xattrs and special-node sidecars.\n', 1)
make_path.write_text(make)

# Keep packaging/DKMS/source-manifest references aligned with the renamed unit.
for path in root.rglob('*'):
    if not path.is_file() or '.git' in path.parts or path == new_meta:
        continue
    try:
        data = path.read_text()
    except UnicodeDecodeError:
        continue
    if 'infiltratorfs_linux_meta.inc' in data:
        path.write_text(data.replace('infiltratorfs_linux_meta.inc', 'infiltratorfs_linux_meta.c'))

# The global manifest rename above also changes the old compositor-order entry;
# remove that entry because Linux metadata is no longer textually composed.
policy = policy_path.read_text()
policy = policy.replace('    infiltratorfs_linux_meta.c\n', '', 1)
expected_old = "grep -Fqx '" + old_line + "' \"$makefile\""
expected_new = "grep -Fqx '" + new_line + "' \"$makefile\""
if expected_old not in policy:
    raise SystemExit('maintainability Kbuild assertion not found')
policy = policy.replace(expected_old, expected_new, 1)
anchor = "test -f \"$kernel/infiltratorfs_directory_tree.c\" || fail 'directory-tree object missing'\n"
extra = anchor + "test -f \"$kernel/infiltratorfs_linux_meta.c\" || fail 'Linux metadata object missing'\n" + \
    "test ! -e \"$kernel/infiltratorfs_linux_meta.inc\" || fail 'Linux metadata regressed to textual include'\n" + \
    "! grep -Fq 'infiltratorfs_linux_meta.inc' \"$rw\" || fail 'RW compositor textually includes Linux metadata'\n"
if anchor not in policy:
    raise SystemExit('maintainability object anchor not found')
policy_path.write_text(policy.replace(anchor, extra, 1))

# Keep source-organization prose accurate without changing behaviour claims.
readme = kernel / 'README.md'
doc = readme.read_text()
doc = doc.replace('and `infiltratorfs_directory_tree.c` owns scalable Format 0.17 directory trees.',
                  '`infiltratorfs_directory_tree.c` owns scalable Format 0.17 directory trees, and `infiltratorfs_linux_meta.c` owns Linux-only sidecar metadata.', 1)
doc = doc.replace('including write-data paths, namespace mutation, Linux metadata, quotas and defragmentation.',
                  'including write-data paths, namespace mutation, quotas and defragmentation.', 1)
readme.write_text(doc)
