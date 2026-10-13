#!/usr/bin/env python3
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
wf = root / '.github' / 'workflows'

# Long destructive/integration qualifications are deliberately manual-only.
manual_long = {
    'heavy-qualification.yml',
    'root-boot-qualification.yml',
    'desktop-formatter-qualification.yml',
}

# This input-producing integration build is allowed to run longer, but only
# when path-gated or manually dispatched.
automatic_long = {
    'desktop-integration-packages.yml',
}

# The native kernel workflow retains deep mounted checks for manual runs.
# Automatic executions are bounded by the dedicated ten-minute watchdog.
external_watchdog = {
    'kernel-module.yml',
}

obsolete = {
    'apply-ci-duration-policy.yml',
    'apply-linux-meta-empty-sidecar-fix.yml',
    'apply-posix-private-cow-fix.yml',
    'apply-special-otrunc-fix.yml',
    'cancel-stale-runs.yml',
    'diagnose-linux-meta-publication.yml',
    'kernel-checksum-cache-object-refactor.yml',
    'qualify-posix-private-inplace.yml',
    'qualify-quota-ctime-isolation.yml',
    'release-0.18.48.yml',
    'release-artifacts.yml',
}


def on_triggers(text: str) -> set[str]:
    try:
        on_block = text.split('on:\n', 1)[1].split('\npermissions:', 1)[0]
    except IndexError as exc:
        raise SystemExit('workflow trigger block could not be parsed') from exc
    return {
        match.group(1)
        for line in on_block.splitlines()
        if (match := re.match(r'^  ([A-Za-z0-9_-]+):\s*$', line))
    }


def enforce_runner_timeouts(name: str, text: str) -> None:
    lines = text.splitlines()
    try:
        jobs = lines.index('jobs:')
    except ValueError as exc:
        raise SystemExit(f'{name}: jobs block missing') from exc

    starts = [
        i for i in range(jobs + 1, len(lines))
        if re.match(r'^  [A-Za-z0-9_-]+:\s*$', lines[i])
    ]
    for n, a in enumerate(starts):
        b = starts[n + 1] if n + 1 < len(starts) else len(lines)
        block = lines[a:b]
        job = block[0].strip()[:-1]
        if not any(line.startswith('    runs-on:') for line in block):
            continue
        timeout = next(
            (line for line in block if line.startswith('    timeout-minutes:')),
            None,
        )
        if timeout is None:
            raise SystemExit(f'{name}:{job}: runner job has no timeout')
        value = timeout.split(':', 1)[1].strip()
        if not value.isdigit():
            raise SystemExit(f'{name}:{job}: automatic timeout must be a fixed integer')
        if int(value) > 10:
            raise SystemExit(f'{name}:{job}: automatic-capable timeout is {value} minutes')


def contains_uncommented(text: str, needle: str) -> bool:
    return any(
        needle in line and not line.lstrip().startswith('#')
        for line in text.splitlines()
    )


workflow_paths = sorted(wf.glob('*.yml')) + sorted(wf.glob('*.yaml'))
if not workflow_paths:
    raise SystemExit('no workflows found')

workflow_names = {path.name for path in workflow_paths}
for name in obsolete:
    if name in workflow_names:
        raise SystemExit(f'obsolete one-shot workflow remains: {name}')
for name in manual_long | automatic_long | external_watchdog:
    if name not in workflow_names:
        raise SystemExit(f'required workflow is missing: {name}')

for path in workflow_paths:
    name = path.name
    text = path.read_text()
    triggers = on_triggers(text)

    if name in manual_long:
        if triggers != {'workflow_dispatch'}:
            raise SystemExit(
                f'{name}: heavyweight qualification must be workflow_dispatch-only; '
                f'found triggers {sorted(triggers)}'
            )
        continue

    if name in external_watchdog:
        continue

    if name in automatic_long:
        if 'push' not in triggers or 'workflow_dispatch' not in triggers or 'paths:' not in text:
            raise SystemExit(
                f'{name}: heavyweight automatic integration build must be '
                'path-gated and manually dispatchable'
            )
        continue

    enforce_runner_timeouts(name, text)

# CMake is the sole semantic release-version authority. There must be no second
# hand-maintained release trigger carrying the same version.

# DKMS source composition also has one authority. Packaging, hosted module
# reproduction and destructive qualification must all consume the same stager.
dkms_manifest = root / 'packaging' / 'kernel-dkms-files.list'
dkms_stager = root / 'packaging' / 'stage-kernel-dkms-source.sh'
if not dkms_manifest.is_file() or not dkms_stager.is_file():
    raise SystemExit('canonical DKMS source manifest/stager is missing')
for consumer in (
    root / 'packaging' / 'build-linux-packages.sh',
    root / '.github' / 'workflows' / 'kernel-module.yml',
    root / 'tests' / 'native-complete-qualification.sh',
):
    if 'stage-kernel-dkms-source.sh' not in consumer.read_text():
        raise SystemExit(f'{consumer.relative_to(root)} bypasses canonical DKMS staging')

if (root / '.github' / 'release-trigger').exists():
    raise SystemExit('duplicate .github/release-trigger release identity remains')

version_helper = root / 'tools' / 'project-version.sh'
if not version_helper.is_file():
    raise SystemExit('canonical project-version resolver is missing')
subprocess.run(['bash', '-n', str(version_helper)], check=True)

release_identity = root / 'tools' / 'release' / 'resolve-request.sh'
if not release_identity.is_file():
    raise SystemExit('semantic release request resolver is missing')
subprocess.run(['bash', '-n', str(release_identity)], check=True)
identity_text = release_identity.read_text()
for required in (
    'tools/project-version.sh CMakeLists.txt',
    'git show HEAD^:CMakeLists.txt',
    'version_is_newer',
    'Release version rollback',
    'emit requested',
    'emit version',
    'emit sha',
):
    if required not in identity_text:
        raise SystemExit(f'release request resolver invariant missing: {required}')
for forbidden in ('.github/release-trigger', 'head_commit.message', 'git log -1 --format=%s'):
    if contains_uncommented(identity_text, forbidden):
        raise SystemExit(f'release request resolver contains obsolete coupling: {forbidden}')

release_candidate = (wf / 'release-candidate-linux.yml').read_text()
release = (wf / 'release-packages.yml').read_text()
for name, text in {
    'release-candidate-linux.yml': release_candidate,
    'release-packages.yml': release,
}.items():
    if 'tools/release/resolve-request.sh' not in text:
        raise SystemExit(f'{name}: release identity is not delegated to the semantic resolver')
    if "steps.release.outputs.requested == 'true'" not in text:
        raise SystemExit(f'{name}: release work is not gated by semantic request identity')
    for forbidden in ('head_commit.message', 'git log -1 --format=%s', '.github/release-trigger'):
        if contains_uncommented(text, forbidden):
            raise SystemExit(f'{name}: release identity contains obsolete coupling: {forbidden}')

# The candidate builds, validates, installs and uploads the final bytes once.
for required in (
    '.github/desktop-integration-release',
    'desktop-integration-ubuntu24.04-',
    'INFILTRATORFS_OS_INTEGRATION_BUNDLE_DIR',
    'INFILTRATORFS_REQUIRE_OS_INTEGRATION',
    'packaging/build-linux-packages.sh build dist',
    'sha256sum -c SHA256SUMS.txt',
    'infiltratorfs-os-integration verify',
    'dkms status -m infiltratorfs -v "$VERSION"',
    'name: InfiltratorFS-Linux',
):
    if required not in release_candidate:
        raise SystemExit(f'release candidate invariant missing: {required}')
for forbidden in (
    'releases?per_page=100',
    'INFILTRATORFS_DESKTOP_BUNDLE_DIR',
    'managed-packages',
):
    if forbidden in release_candidate:
        raise SystemExit(f'release candidate contains dynamic or retired input path: {forbidden}')

# Publication consumes the exact candidate artifact. It may re-check bytes and
# tag/release state, but must never rebuild or mutate an existing release.
for required in (
    '"Native Linux kernel module"',
    '"Linux root-volume qualification"',
    '"Linux metadata qualification"',
    '"Release candidate Linux"',
    '--name InfiltratorFS-Linux',
    'git tag --annotate',
    'gh release download',
    'cmp -s',
    'sha256sum -c SHA256SUMS.txt',
    'tools/project-version.sh CMakeLists.txt',
):
    if required not in release:
        raise SystemExit(f'release publication invariant missing: {required}')
for forbidden in (
    'apt-get build-dep',
    'cmake --build',
    'git rev-parse origin/main',
    'TRIGGER_RUN_ID',
    'gate_run_id',
    'gate_updated_at',
    'InfiltratorFS-release-assets',
    'gh release edit',
    '--clobber',
):
    if forbidden in release:
        raise SystemExit(f'release publisher contains obsolete coupling: {forbidden}')

# The native kernel workflow retains longer internal diagnostic allowances for
# manual runs; automatic runs are still stopped by this one focused watchdog.
watchdog = (wf / 'automatic-ci-watchdog.yml').read_text()
for required in (
    'workflows: ["Native Linux kernel module"]',
    'types: [in_progress]',
    "github.event.workflow_run.event != 'workflow_dispatch'",
    'timeout-minutes: 10',
    'target_seconds=570',
    '/actions/runs/${TARGET_RUN_ID}/cancel',
):
    if required not in watchdog:
        raise SystemExit(f'automatic kernel watchdog policy missing: {required}')

kernel = (wf / 'kernel-module.yml').read_text()
if 'name: Native Linux kernel module' not in kernel:
    raise SystemExit('native kernel workflow identity changed')
if 'workflow_dispatch:' not in kernel:
    raise SystemExit('native kernel workflow must remain manually dispatchable')
if contains_uncommented(kernel, '.github/release-trigger'):
    raise SystemExit('native kernel workflow retains deleted release-trigger coupling')
if 'push' not in on_triggers(kernel):
    raise SystemExit('native kernel workflow automatic trigger unexpectedly removed')

print('CI duration policy: PASS')
