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
text = maint.read_text()
old = '''grep -Fq 'kernel/infiltratorfs_orphan_scan.c \\' "$root/.github/workflows/kernel-module.yml" || \\
    fail 'DKMS qualification source root omits orphan scanner component'
'''
new = '''dkms_manifest="$root/packaging/kernel-dkms-files.list"
test -f "$dkms_manifest" || fail 'canonical DKMS source manifest missing'
grep -Fqx $'kernel/infiltratorfs_orphan_scan.c\\tinfiltratorfs_orphan_scan.c' "$dkms_manifest" || \\
    fail 'canonical DKMS source manifest omits orphan scanner component'
'''
if text.count(old) != 1:
    raise SystemExit('maintainability guard: old workflow-coupled orphan assertion not found exactly once')
text = text.replace(old, new, 1)
old = '''package_builder="$root/packaging/build-linux-packages.sh"
test -f "$package_builder" || fail 'Linux package builder missing'
module_objects="$(sed -nE 's/^infiltratorfs-y := (.*)$/\\1/p' "$makefile")"
test -n "$module_objects" || fail 'could not resolve Kbuild object list'
for object in $module_objects; do
    source="${object%.o}.c"
    grep -Fq "$source" "$package_builder" || \\
        fail "DKMS package source list omits $source"
done
'''
new = '''dkms_stager="$root/packaging/stage-kernel-dkms-source.sh"
test -x "$dkms_stager" || fail 'canonical DKMS source stager missing or not executable'
module_objects="$(sed -nE 's/^infiltratorfs-y := (.*)$/\\1/p' "$makefile")"
test -n "$module_objects" || fail 'could not resolve Kbuild object list'
for object in $module_objects; do
    source="${object%.o}.c"
    grep -Fqx "kernel/${source}"$'\\t'"${source}" "$dkms_manifest" || \\
        fail "canonical DKMS source manifest omits kernel/${source} -> ${source}"
done
for consumer in \\
    "$root/packaging/build-linux-packages.sh" \\
    "$root/.github/workflows/kernel-module.yml" \\
    "$root/tests/native-complete-qualification.sh"; do
    grep -Fq 'stage-kernel-dkms-source.sh' "$consumer" || \\
        fail "$(realpath --relative-to="$root" "$consumer") bypasses canonical DKMS staging"
done
'''
if text.count(old) != 1:
    raise SystemExit('maintainability guard: old package-builder source-list assertion not found exactly once')
maint.write_text(text.replace(old, new, 1))

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
