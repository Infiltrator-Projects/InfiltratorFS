#!/usr/bin/env python3
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
wf = root / '.github' / 'workflows'

# These are intentionally heavyweight and may exceed ten minutes, but only
# because they are manual-only qualifications. Any new long workflow must be
# deliberately added here and must still pass the dispatch-only trigger check.
manual_long = {
    'heavy-qualification.yml',
    'root-boot-qualification.yml',
    'desktop-formatter-qualification.yml',
}

automatic_long = {
    'desktop-integration-packages.yml',
}

# The native kernel workflow is the one automatic exception to an in-file job
# timeout because manual runs are intentionally allowed to continue deeply.
# Automatic executions are bounded by automatic-ci-watchdog.yml at 570 seconds.
external_watchdog = {
    'kernel-module.yml',
}

obsolete = {
    'apply-ci-duration-policy.yml',
    'apply-linux-meta-empty-sidecar-fix.yml',
    'apply-posix-private-cow-fix.yml',
    'apply-special-otrunc-fix.yml',
    'diagnose-linux-meta-publication.yml',
    'kernel-checksum-cache-object-refactor.yml',
    'qualify-posix-private-inplace.yml',
    'qualify-quota-ctime-isolation.yml',
    'release-0.18.48.yml',
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
        minutes = int(timeout.split(':', 1)[1].strip())
        if minutes > 10:
            raise SystemExit(f'{name}:{job}: automatic-capable timeout is {minutes} minutes')


def contains_uncommented(text: str, needle: str) -> bool:
    return any(
        needle in line and not line.lstrip().startswith('#')
        for line in text.splitlines()
    )


# Fail closed over the entire workflow directory. A brand-new workflow is
# automatically checked; it cannot escape just because somebody forgot to add
# its filename to a hand-maintained "automatic" list.
workflow_paths = sorted(wf.glob('*.yml')) + sorted(wf.glob('*.yaml'))
if not workflow_paths:
    raise SystemExit('no workflows found')

workflow_names = {path.name for path in workflow_paths}
missing_manual = manual_long - workflow_names
if missing_manual:
    raise SystemExit(f'manual long workflow(s) missing: {sorted(missing_manual)}')
missing_watchdog = external_watchdog - workflow_names
if missing_watchdog:
    raise SystemExit(f'externally watched workflow(s) missing: {sorted(missing_watchdog)}')
missing_automatic_long = automatic_long - workflow_names
if missing_automatic_long:
    raise SystemExit(f'automatic long workflow(s) missing: {sorted(missing_automatic_long)}')

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
            raise SystemExit(f'{name}: heavyweight automatic integration build must be path-gated and manually dispatchable')
        continue

    # Every other current or future runner-backed workflow is capped at ten
    # minutes, regardless of whether its trigger is push, schedule,
    # workflow_run, repository_dispatch, workflow_dispatch, or something new.
    enforce_runner_timeouts(name, text)

# The release trigger is only an explicit release identity. Qualification state
# belongs to CI evidence and release artifacts, not to a hand-maintained ledger.
release_trigger = (root / '.github' / 'release-trigger').read_text().splitlines()
if len(release_trigger) != 1 or not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', release_trigger[0]):
    raise SystemExit('release trigger must contain exactly one semantic version line')

# Release intent is semantic, not prose. One helper owns the definition: the
# one-line release version must match CMake, and a release is requested only
# when that version differs from the first parent's trigger version.
release_identity = root / 'tools' / 'release' / 'resolve-request.sh'
if not release_identity.is_file():
    raise SystemExit('semantic release request resolver is missing')
subprocess.run(['bash', '-n', str(release_identity)], check=True)
identity_text = release_identity.read_text()
for required in (
    'HEAD^:.github/release-trigger',
    'trigger" == "$version',
    'trigger" != "$previous',
    'emit requested',
    'emit version',
    'emit sha',
):
    if required not in identity_text:
        raise SystemExit(f'release request resolver invariant missing: {required}')

release_candidate = (wf / 'release-candidate-linux.yml').read_text()
release_artifacts = (wf / 'release-artifacts.yml').read_text()
release = (wf / 'release-packages.yml').read_text()
release_workflows = {
    'release-candidate-linux.yml': release_candidate,
    'release-artifacts.yml': release_artifacts,
    'release-packages.yml': release,
}
for name, text in release_workflows.items():
    if 'tools/release/resolve-request.sh' not in text:
        raise SystemExit(f'{name}: release identity is not delegated to the semantic resolver')
    if "steps.release.outputs.requested == 'true'" not in text:
        raise SystemExit(f'{name}: release work is not gated by semantic request identity')
    for forbidden in ('head_commit.message', 'git log -1 --format=%s'):
        if contains_uncommented(text, forbidden):
            raise SystemExit(f'{name}: release identity still depends on commit prose: {forbidden}')

# The release candidate owns all inputs needed to produce the final shipping
# Linux artifacts. The desktop integration input is pinned in source; it must
# not be selected dynamically at promotion time.
for required in (
    '.github/desktop-integration-release',
    'desktop-integration-ubuntu24.04-',
    'INFILTRATORFS_OS_INTEGRATION_BUNDLE_DIR',
    'INFILTRATORFS_REQUIRE_OS_INTEGRATION',
    'packaging/build-linux-packages.sh build dist',
    'SHA256SUMS.txt',
):
    if required not in release_candidate:
        raise SystemExit(f'release candidate invariant missing: {required}')
for forbidden in (
    'releases?per_page=100',
    'tail -n 1',
    'INFILTRATORFS_DESKTOP_BUNDLE_DIR',
    'managed-packages',
):
    if forbidden in release_candidate:
        raise SystemExit(f'release candidate contains dynamic or retired input path: {forbidden}')

# Promotion may validate and install the exact candidate artifacts, but it must
# never rebuild them, re-resolve their inputs, or depend on main still pointing
# at the already-tested SHA.
for required in (
    'gh run download',
    'SHA256SUMS.txt',
    'sha256sum -c SHA256SUMS.txt',
    'ubuntu24.04-mint22-bundled',
    'infiltratorfs-os-integration verify',
):
    if required not in release_artifacts:
        raise SystemExit(f'release artifact promotion invariant missing: {required}')
for forbidden in (
    'apt-get build-dep',
    'packaging/build-linux-packages.sh',
    'INFILTRATORFS_OS_INTEGRATION_BUNDLE_DIR',
    'gh release download',
    'INFILTRATORFS_DESKTOP_BUNDLE_DIR',
    'managed-packages',
    'git rev-parse origin/main',
):
    if forbidden in release_artifacts:
        raise SystemExit(f'release artifact promotion is coupled to build/input/main state: {forbidden}')

# Publication consumes exact promoted bytes. It must be retry-safe and must not
# elect one workflow run as a special "last finisher"; concurrency plus
# idempotent tag/release verification owns duplicate completion events.
for required in (
    '"Native Linux kernel module"',
    '"Linux root-volume qualification"',
    '"Linux metadata qualification"',
    '"Release artifacts"',
    'gh run download',
    'git tag --annotate',
    'gh release download',
    'cmp -s',
    'sha256sum -c SHA256SUMS.txt',
):
    if required not in release:
        raise SystemExit(f'release publication invariant missing: {required}')
for forbidden in (
    'apt-get build-dep',
    'cmake --build',
    'timeout-minutes: 12',
    'git rev-parse origin/main',
    'TRIGGER_RUN_ID',
    'gate_run_id',
    'gate_updated_at',
):
    if forbidden in release:
        raise SystemExit(f'release publisher contains obsolete coupling: {forbidden}')

# The native kernel workflow deliberately retains deeper mounted steps with
# longer internal diagnostic allowances. Automatic runs are bounded externally:
# only manual workflow_dispatch executions may continue beyond ten minutes.
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
if 'push' not in on_triggers(kernel):
    raise SystemExit('native kernel workflow automatic trigger unexpectedly removed')

for name in obsolete:
    if (wf / name).exists():
        raise SystemExit(f'obsolete one-shot workflow remains: {name}')

print('CI duration policy: PASS')
