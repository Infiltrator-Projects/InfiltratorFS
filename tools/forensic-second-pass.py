#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    if text.count(old) != 1:
        raise SystemExit(f'{path}: expected exactly one occurrence of {old!r}, found {text.count(old)}')
    p.write_text(text.replace(old, new, 1))


maint = Path('tests/native-kernel-maintainability-policy.sh')
lines = maint.read_text().splitlines(keepends=True)

orphan_hits = [i for i, line in enumerate(lines)
               if 'kernel/infiltratorfs_orphan_scan.c' in line
               and '.github/workflows/kernel-module.yml' in line]
if len(orphan_hits) != 1:
    raise SystemExit(f'maintainability guard: expected one workflow-coupled orphan assertion, found {len(orphan_hits)}')
i = orphan_hits[0]
if i + 1 >= len(lines) or 'DKMS qualification source root omits orphan scanner component' not in lines[i + 1]:
    raise SystemExit('maintainability guard: orphan assertion continuation changed unexpectedly')
lines[i:i + 2] = [
    'dkms_manifest="$root/packaging/kernel-dkms-files.list"\n',
    'test -f "$dkms_manifest" || fail \'canonical DKMS source manifest missing\'\n',
    'grep -Fqx $\'kernel/infiltratorfs_orphan_scan.c\\tinfiltratorfs_orphan_scan.c\' "$dkms_manifest" || \\\n',
    '    fail \'canonical DKMS source manifest omits orphan scanner component\'\n',
]

package_hits = [i for i, line in enumerate(lines) if line.startswith('package_builder="$root/packaging/build-linux-packages.sh"')]
if len(package_hits) != 1:
    raise SystemExit(f'maintainability guard: expected one duplicated package-builder DKMS block, found {len(package_hits)}')
start = package_hits[0]
end = None
for j in range(start, min(start + 20, len(lines))):
    if lines[j] == 'done\n':
        end = j
        break
if end is None:
    raise SystemExit('maintainability guard: package-builder DKMS block terminator not found')
block = ''.join(lines[start:end + 1])
for marker in ('module_objects=', 'for object in $module_objects', 'grep -Fq "$source" "$package_builder"'):
    if marker not in block:
        raise SystemExit(f'maintainability guard: package-builder block lost expected marker {marker!r}')
lines[start:end + 1] = [
    'dkms_stager="$root/packaging/stage-kernel-dkms-source.sh"\n',
    'test -x "$dkms_stager" || fail \'canonical DKMS source stager missing or not executable\'\n',
    'module_objects="$(sed -nE \'s/^infiltratorfs-y := (.*)$/\\1/p\' "$makefile")"\n',
    'test -n "$module_objects" || fail \'could not resolve Kbuild object list\'\n',
    'for object in $module_objects; do\n',
    '    source="${object%.o}.c"\n',
    '    grep -Fqx "kernel/${source}"$\'\\t\'"${source}" "$dkms_manifest" || \\\n',
    '        fail "canonical DKMS source manifest omits kernel/${source} -> ${source}"\n',
    'done\n',
    'for consumer in \\\n',
    '    "$root/packaging/build-linux-packages.sh" \\\n',
    '    "$root/.github/workflows/kernel-module.yml" \\\n',
    '    "$root/tests/native-complete-qualification.sh"; do\n',
    '    grep -Fq \'stage-kernel-dkms-source.sh\' "$consumer" || \\\n',
    '        fail "$(realpath --relative-to="$root" "$consumer") bypasses canonical DKMS staging"\n',
    'done\n',
]
maint.write_text(''.join(lines))

replace_once(
    'docs/QUALIFICATION.md',
    'This file is the **single authoritative exact-source evidence record** for InfiltratorFS. It records what was actually exercised on named commits and workflows. It is not a feature list; feature completion belongs in `ROADMAP.md`.\n\nHistorical results apply only to the source commit on which they ran. A later green portable CI run does not silently inherit an older mounted, heavy or destructive qualification result.\n',
    'This file is the **authoritative qualification contract and curated milestone/manual evidence ledger** for InfiltratorFS. Live automatic exact-source evidence is owned by the immutable GitHub Actions checks attached to the exact commit and, for a published version, by its immutable tag and GitHub Release. This file is not a feature list; feature completion belongs in `ROADMAP.md`.\n\nHistorical results recorded here apply only to the source commit on which they ran. They are deliberately not a hand-maintained mirror of every automatic workflow run. A later green portable CI run does not silently inherit an older mounted, heavy or destructive qualification result.\n')
replace_once(
    'docs/QUALIFICATION.md',
    '## Current development evidence boundary\n',
    '## Curated development milestone evidence\n\nThe entries below preserve useful milestone evidence. For the current source revision, automatic qualification state must be read from the checks attached to that exact Git commit; absence of a newer hand-entered row here does not imply absence of automatic qualification.\n\n')
replace_once(
    'docs/README.md',
    '- [`QUALIFICATION.md`](QUALIFICATION.md) — released-baseline and exact-source qualification ledger; the authority for what evidence exists for a specific source revision.\n',
    '- [`QUALIFICATION.md`](QUALIFICATION.md) — qualification classes, policy, and curated milestone/manual evidence; live automatic exact-SHA evidence is owned by the GitHub Actions checks attached to that commit and published release identity by its immutable tag/release.\n')
replace_once(
    'CHANGELOG.md',
    '- Clarify that Windows source-shape guards are not a substitute for mounted Windows and Driver Verifier qualification.\n',
    '- Clarify that Windows source-shape guards are not a substitute for mounted Windows and Driver Verifier qualification.\n- Make the native maintainability guard consume the canonical DKMS manifest instead of asserting the removed duplicated source lists, and make GitHub checks—not a stale hand-maintained table—the live authority for automatic exact-SHA qualification evidence.\n')
