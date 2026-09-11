#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path

pkg = Path('packaging/build-linux-packages.sh')
helper = Path('tools/kernel-checksum-cache-object-refactor.py')

text = pkg.read_text()
old_required = '    "usr/src/infiltratorfs-${package_version}/infiltratorfs_checksum_cache.c infiltratorfs_rw_data.inc$" \\\n'
new_required = (
    '    "usr/src/infiltratorfs-${package_version}/infiltratorfs_checksum_cache.c$" \\\n'
    '    "usr/src/infiltratorfs-${package_version}/infiltratorfs_rw_data.inc$" \\\n'
)
if text.count(old_required) != 1:
    raise SystemExit('broken Debian contents validation marker not found exactly once')
text = text.replace(old_required, new_required, 1)

old_run = '        kernel/infiltratorfs_rw_legacy.inc kernel/infiltratorfs_checksum_cache.c infiltratorfs_rw_data.inc \\\n'
new_run = (
    '        kernel/infiltratorfs_rw_legacy.inc kernel/infiltratorfs_checksum_cache.c \\\n'
    '        kernel/infiltratorfs_rw_data.inc \\\n'
)
if text.count(old_run) != 1:
    raise SystemExit('broken native installer validation marker not found exactly once')
text = text.replace(old_run, new_run, 1)
pkg.write_text(text)

# Harden the reusable extractor: package/source manifests must be edited by
# path-aware cases instead of blindly prefixing a token before every textual
# rw_data occurrence. The latter can concatenate two filenames inside quoted
# validation patterns and silently create impossible package checks.
h = helper.read_text()
old = '''for p in [
    root / 'packaging/build-linux-packages.sh',
    root / '.github/workflows/kernel-module.yml',
    root / 'tests/native-complete-qualification.sh',
]:
    if not p.exists():
        continue
    text = p.read_text()
    if 'infiltratorfs_checksum_cache.c' in text:
        continue
    marker = 'infiltratorfs_rw_data.inc'
    if marker not in text:
        raise SystemExit(f'cannot add checksum cache to source manifest {p}')
    p.write_text(text.replace(marker, 'infiltratorfs_checksum_cache.c ' + marker))
'''
new = '''# Source-manifest updates must remain path-aware. Do not globally prefix the
# rw_data token: quoted package validation patterns require distinct path
# entries, and concatenating filenames there creates an impossible check.
for p in [
    root / 'packaging/build-linux-packages.sh',
    root / '.github/workflows/kernel-module.yml',
    root / 'tests/native-complete-qualification.sh',
]:
    if not p.exists():
        continue
    text = p.read_text()
    if 'infiltratorfs_checksum_cache.c' in text:
        continue
    if p.name == 'build-linux-packages.sh':
        install_anchor = '            infiltratorfs_rw_legacy.inc infiltratorfs_rw_data.inc \\\\n'
        install_repl = ('            infiltratorfs_rw_legacy.inc infiltratorfs_checksum_cache.c \\\\n'
                        '            infiltratorfs_rw_data.inc \\\\n')
        required_anchor = '    "usr/src/infiltratorfs-${package_version}/infiltratorfs_rw_data.inc$" \\\\n'
        required_repl = ('    "usr/src/infiltratorfs-${package_version}/infiltratorfs_checksum_cache.c$" \\\\n'
                         '    "usr/src/infiltratorfs-${package_version}/infiltratorfs_rw_data.inc$" \\\\n')
        run_anchor = '        kernel/infiltratorfs_rw_legacy.inc kernel/infiltratorfs_rw_data.inc \\\\n'
        run_repl = ('        kernel/infiltratorfs_rw_legacy.inc kernel/infiltratorfs_checksum_cache.c \\\\n'
                    '        kernel/infiltratorfs_rw_data.inc \\\\n')
        for anchor, repl in ((install_anchor, install_repl),
                             (required_anchor, required_repl),
                             (run_anchor, run_repl)):
            if anchor not in text:
                raise SystemExit(f'cannot add checksum cache to package manifest {p}: {anchor!r}')
            text = text.replace(anchor, repl, 1)
        p.write_text(text)
        continue
    marker = 'infiltratorfs_rw_data.inc'
    if marker not in text:
        raise SystemExit(f'cannot add checksum cache to source manifest {p}')
    p.write_text(text.replace(marker, 'infiltratorfs_checksum_cache.c ' + marker))
'''
if h.count(old) != 1:
    raise SystemExit('checksum refactor manifest block not found exactly once')
helper.write_text(h.replace(old, new, 1))

fixed = pkg.read_text()
# Only reject the impossible quoted package-content filename and the native
# installer path that lost its kernel/ prefix.  A shell source list may
# legitimately place checksum_cache.c and rw_data.inc beside one another.
invalid_required = 'usr/src/infiltratorfs-${package_version}/infiltratorfs_checksum_cache.c infiltratorfs_rw_data.inc$'
invalid_run = 'kernel/infiltratorfs_checksum_cache.c infiltratorfs_rw_data.inc'
if invalid_required in fixed:
    raise SystemExit('broken Debian contents validation path remains')
if invalid_run in fixed:
    raise SystemExit('broken native installer validation path remains')

required_checks = (
    'usr/src/infiltratorfs-${package_version}/infiltratorfs_checksum_cache.c$',
    'usr/src/infiltratorfs-${package_version}/infiltratorfs_rw_data.inc$',
    'kernel/infiltratorfs_checksum_cache.c',
    'kernel/infiltratorfs_rw_data.inc',
)
for marker in required_checks:
    if marker not in fixed:
        raise SystemExit(f'expected repaired package marker missing: {marker}')

print('Package modularization manifests repaired.')
