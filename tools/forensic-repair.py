#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import re


def replace_exact(path, old, new, count=1):
    p = Path(path)
    text = p.read_text()
    found = text.count(old)
    if found != count:
        raise SystemExit(f'{path}: expected {count} occurrences, found {found}: {old!r}')
    p.write_text(text.replace(old, new))


replace_exact('CMakeLists.txt',
              'project(InfiltratorFS VERSION 0.18.97 LANGUAGES C)\n# Development qualification anchor: 0.18.97',
              'project(InfiltratorFS VERSION 0.18.98 LANGUAGES C)\n# Development qualification anchor: 0.18.98')
replace_exact('README.md', '**Current source:** 0.18.97<br>', '**Current source:** 0.18.98<br>')
replace_exact('windows-driver/infiltratorfs-native.inf',
              'DriverVer=10/07/2026,0.18.97.0',
              'DriverVer=10/13/2026,0.18.98.0')

design = Path('docs/IAC1-2026-HARDWARE-DESIGN.md')
text = design.read_text()
for old, new in {
    'N-1 online-logical-CPU budget': 'N-1 online-physical-core budget',
    'derived from the online logical CPU count': 'derived from the online physical-core count',
    'On a 4-logical-CPU machine, up to 3 CPU-bound filesystem worker units may run concurrently.':
        'On a 4-physical-core machine, up to 3 CPU-bound filesystem worker units may run concurrently.',
    'On a 64-logical-CPU machine, up to 63 may run concurrently.':
        'On a 64-physical-core machine, up to 63 may run concurrently.',
}.items():
    if old not in text:
        raise SystemExit(f'IAC1 hardware design: expected text missing: {old!r}')
    text = text.replace(old, new)
design.write_text(text)

kernel_wf = Path('.github/workflows/kernel-module.yml')
text = kernel_wf.read_text()
stale = "      - '.github/release-trigger'\n"
if text.count(stale) != 1:
    raise SystemExit('kernel workflow: stale release-trigger path was not unique')
text = text.replace(stale, '')
kernel_wf.write_text(text)

win_policy = Path('tests/native-windows-driver-policy.sh')
text = win_policy.read_text()
marker = 'set -euo pipefail\n'
note = ('set -euo pipefail\n'
        '# This is a source-shape regression guard, not mounted Windows qualification.\n'
        '# Runtime filesystem semantics, teardown races and Driver Verifier behaviour must\n'
        '# be established by the mounted Windows qualification tracked in ROADMAP.md.\n')
if text.count(marker) != 1:
    raise SystemExit('Windows policy: shell prologue changed unexpectedly')
text = text.replace(marker, note, 1)
brittle = "grep -Fq 'Signal while QueueLock still protects Item lifetime' \"$driver\" || fail 'request completion is not lifetime-safe against timeout'\n"
if text.count(brittle) != 1:
    raise SystemExit('Windows policy: exact-comment pseudo-proof changed unexpectedly')
text = text.replace(brittle, '')
win_policy.write_text(text)

changelog = Path('CHANGELOG.md')
text = changelog.read_text()
anchor = 'This file records user-visible, compatibility, architecture and validation changes for InfiltratorFS.\n\n'
if text.count(anchor) != 1:
    raise SystemExit('CHANGELOG heading changed unexpectedly')
entries = '''## 0.18.98 — 2026-10-13
- Complete a philosophy-governed forensic architecture, test, packaging and release review without changing Format 0.18 semantics.
- Centralize DKMS source staging so packaging, hosted kernel reproduction and destructive qualification consume one authoritative component manifest.
- Strengthen release identity so source cannot reuse an already-published semantic version at a different commit, and reject version rollback relative to reachable releases.
- Remove the retired `.github/release-trigger` path from native CI and correct the IAC1 hardware document to the implemented physical-core N-1 policy.
- Clarify that Windows source-shape guards are not a substitute for mounted Windows and Driver Verifier qualification.
- On-disk Format remains 0.18.

## 0.18.97 — 2026-10-07
- Build final Linux release artifacts once from pinned inputs and publish the exact qualified bytes instead of rebuilding during publication.
- Remove duplicated release-qualification coupling and verify exact artifact lineage through checksums and byte-for-byte post-publication comparison.
- On-disk Format remains 0.18.

## 0.18.96 — 2026-10-07
- Simplify the release qualification pipeline while retaining exact-source native kernel, root-volume, Linux metadata and package qualification prerequisites.
- Decouple root-volume semantic qualification from the separately pinned desktop integration bundle.
- On-disk Format remains 0.18.

## 0.18.95 — 2026-10-07
- Restore the single-package Linux desktop integration model and publish corrected integrated Linux packages.
- Decouple root-volume filesystem semantics from desktop bundle mechanics and prove installation of the released package.
- Remove a duplicated storage-policy fallback check without weakening persistent storage policy semantics.
- On-disk Format remains 0.18.

'''
if '## 0.18.95 ' in text or '## 0.18.98 ' in text:
    raise SystemExit('CHANGELOG already contains forensic/backfill entries')
changelog.write_text(text.replace(anchor, anchor + entries, 1))

manifest = Path('packaging/kernel-dkms-files.list')
manifest.write_text('''# repository-source\tdkms-name
kernel/Makefile\tMakefile
kernel/infiltratorfs_core.c\tinfiltratorfs_core.c
kernel/infiltratorfs_cpu.c\tinfiltratorfs_cpu.c
kernel/infiltratorfs_crypto.c\tinfiltratorfs_crypto.c
kernel/infiltratorfs_security.c\tinfiltratorfs_security.c
kernel/infiltratorfs_extension.c\tinfiltratorfs_extension.c
kernel/infiltratorfs_internal.h\tinfiltratorfs_internal.h
kernel/infiltratorfs_format.h\tinfiltratorfs_format.h
kernel/infiltratorfs_allocation_map.c\tinfiltratorfs_allocation_map.c
kernel/infiltratorfs_allocation_publish.c\tinfiltratorfs_allocation_publish.c
kernel/infiltratorfs_parallel_alloc.c\tinfiltratorfs_parallel_alloc.c
kernel/infiltratorfs_index_tree.c\tinfiltratorfs_index_tree.c
kernel/infiltratorfs_extent_tree.c\tinfiltratorfs_extent_tree.c
kernel/infiltratorfs_directory_tree.c\tinfiltratorfs_directory_tree.c
kernel/infiltratorfs_checksum_cache.c\tinfiltratorfs_checksum_cache.c
kernel/infiltratorfs_checksum_store.c\tinfiltratorfs_checksum_store.c
kernel/infiltratorfs_locator_cache.c\tinfiltratorfs_locator_cache.c
kernel/infiltratorfs_linux_meta_codec.c\tinfiltratorfs_linux_meta_codec.c
kernel/infiltratorfs_shared_ownership.c\tinfiltratorfs_shared_ownership.c
kernel/infiltratorfs_orphan_scan.c\tinfiltratorfs_orphan_scan.c
kernel/infiltratorfs_name_policy.c\tinfiltratorfs_name_policy.c
kernel/infiltratorfs_media.c\tinfiltratorfs_media.c
kernel/infiltratorfs_rw.inc\tinfiltratorfs_rw.inc
kernel/infiltratorfs_rw_legacy.inc\tinfiltratorfs_rw_legacy.inc
kernel/infiltratorfs_rw_data.inc\tinfiltratorfs_rw_data.inc
kernel/infiltratorfs_rw_namespace.inc\tinfiltratorfs_rw_namespace.inc
kernel/infiltratorfs_rw_namespace_exchange.inc\tinfiltratorfs_rw_namespace_exchange.inc
kernel/infiltratorfs_read_cache.c\tinfiltratorfs_read_cache.c
kernel/infiltratorfs_pagecache.c\tinfiltratorfs_pagecache.c
kernel/infiltratorfs_linux_meta.inc\tinfiltratorfs_linux_meta.inc
kernel/infiltratorfs_resize.c\tinfiltratorfs_resize.c
kernel/infiltratorfs_quota.inc\tinfiltratorfs_quota.inc
kernel/infiltratorfs_defrag.inc\tinfiltratorfs_defrag.inc
kernel/infiltratorfs_ioctl.h\tinfiltratorfs_ioctl.h
include/infilfs/iac1.h\tiac1.h
''')

helper = Path('packaging/stage-kernel-dkms-source.sh')
helper.write_text('''#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
destination="${1:?DKMS destination directory required}"
manifest="$root/packaging/kernel-dkms-files.list"

[[ -f "$manifest" ]] || { echo "DKMS source manifest is missing: $manifest" >&2; exit 1; }
mkdir -p "$destination"
declare -A targets=()
while IFS=$'\\t' read -r source target; do
    [[ -n "$source" ]] || continue
    [[ "$source" == \#* ]] && continue
    [[ -n "$target" && "$target" != */* ]] || {
        echo "Invalid DKMS manifest target for $source: ${target:-missing}" >&2
        exit 1
    }
    [[ -z "${targets[$target]+x}" ]] || {
        echo "Duplicate DKMS manifest target: $target" >&2
        exit 1
    }
    [[ -f "$root/$source" ]] || {
        echo "DKMS manifest source is missing: $source" >&2
        exit 1
    }
    targets[$target]=1
    install -m 0644 "$root/$source" "$destination/$target"
done < "$manifest"

(( ${#targets[@]} > 0 )) || { echo 'DKMS source manifest is empty.' >&2; exit 1; }
''')
helper.chmod(0o755)

package = Path('packaging/build-linux-packages.sh')
text = package.read_text()
pattern = re.compile(r"# DKMS source must be self-contained\..*?install -m 0644 \"include/infilfs/iac1\.h\" \"\$dkms_root/iac1\.h\"\n", re.S)
replacement = '''# DKMS source must be self-contained. One manifest/stager owns the exact source
# set used by packages, hosted DKMS reproduction and destructive qualification.
dkms_root="$package_root/usr/src/infiltratorfs-$package_version"
bash packaging/stage-kernel-dkms-source.sh "$dkms_root"
'''
text, n = pattern.subn(replacement, text, count=1)
if n != 1:
    raise SystemExit('package builder: could not replace duplicated DKMS source list')
package.write_text(text)

complete = Path('tests/native-complete-qualification.sh')
text = complete.read_text()
pattern = re.compile(r'dkms_copy="\$WORK/dkms-source"\nmkdir -p "\$dkms_copy"\nfor file in .*?cp "\$ROOT/include/infilfs/iac1\.h" "\$dkms_copy/iac1\.h"\n', re.S)
replacement = '''dkms_copy="$WORK/dkms-source"
bash "$ROOT/packaging/stage-kernel-dkms-source.sh" "$dkms_copy"
'''
text, n = pattern.subn(replacement, text, count=1)
if n != 1:
    raise SystemExit('complete qualification: could not replace duplicated DKMS source list')
complete.write_text(text)

kernel = Path('.github/workflows/kernel-module.yml')
text = kernel.read_text()
pattern = re.compile(r'          dkms_src="\$\(mktemp -d\)"\n          trap .*?          cp include/infilfs/iac1\.h "\$dkms_src/iac1\.h"\n', re.S)
replacement = '''          dkms_src="$(mktemp -d)"
          trap 'rm -rf "$dkms_src"' EXIT
          bash packaging/stage-kernel-dkms-source.sh "$dkms_src"
'''
text, n = pattern.subn(replacement, text, count=1)
if n != 1:
    raise SystemExit('kernel workflow: could not replace duplicated DKMS source list')
kernel.write_text(text)

policy = Path('tests/ci-duration-policy.py')
text = policy.read_text()
anchor = "# CMake is the sole semantic release-version authority. There must be no second\n# hand-maintained release trigger carrying the same version.\n"
insert = anchor + "\n# DKMS source composition also has one authority. Packaging, hosted module\n# reproduction and destructive qualification must all consume the same stager.\ndkms_manifest = root / 'packaging' / 'kernel-dkms-files.list'\ndkms_stager = root / 'packaging' / 'stage-kernel-dkms-source.sh'\nif not dkms_manifest.is_file() or not dkms_stager.is_file():\n    raise SystemExit('canonical DKMS source manifest/stager is missing')\nfor consumer in (\n    root / 'packaging' / 'build-linux-packages.sh',\n    root / '.github' / 'workflows' / 'kernel-module.yml',\n    root / 'tests' / 'native-complete-qualification.sh',\n):\n    if 'stage-kernel-dkms-source.sh' not in consumer.read_text():\n        raise SystemExit(f'{consumer.relative_to(root)} bypasses canonical DKMS staging')\n\n"
if text.count(anchor) != 1:
    raise SystemExit('CI policy: DKMS insertion anchor changed unexpectedly')
text = text.replace(anchor, insert, 1)
old_required = "    '\"$version\" != \"$previous\"',\n"
if text.count(old_required) != 1:
    raise SystemExit('CI policy: release-resolver assertion changed unexpectedly')
text = text.replace(old_required, "    'version_is_newer',\n    'Release version rollback',\n", 1)
kernel_guard = "if 'workflow_dispatch:' not in kernel:\n    raise SystemExit('native kernel workflow must remain manually dispatchable')\n"
if text.count(kernel_guard) != 1:
    raise SystemExit('CI policy: kernel trigger assertion anchor changed unexpectedly')
text = text.replace(kernel_guard, kernel_guard + "if contains_uncommented(kernel, '.github/release-trigger'):\n    raise SystemExit('native kernel workflow retains deleted release-trigger coupling')\n", 1)
policy.write_text(text)

resolver = Path('tools/release/resolve-request.sh')
text = resolver.read_text()
old = '''requested=false
if [[ -n "$previous" && "$version" != "$previous" ]]; then
    requested=true
fi
'''
new = '''version_is_newer() {
    local older="$1"
    local newer="$2"
    [[ "$older" != "$newer" ]] &&
        [[ "$(printf '%s\\n%s\\n' "$older" "$newer" | sort -V | head -n1)" == "$older" ]]
}

requested=false
if [[ -n "$previous" ]]; then
    if [[ "$version" == "$previous" ]]; then
        :
    elif version_is_newer "$previous" "$version"; then
        requested=true
    else
        echo "Release version rollback: source $version is older than reachable release $previous" >&2
        exit 1
    fi
fi
'''
if text.count(old) != 1:
    raise SystemExit('release resolver: request block changed unexpectedly')
resolver.write_text(text.replace(old, new, 1))
