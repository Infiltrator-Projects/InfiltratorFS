#!/usr/bin/env python3
from pathlib import Path
import re

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

    # Every other current or future runner-backed workflow is capped at ten
    # minutes, regardless of whether its trigger is push, schedule,
    # workflow_run, repository_dispatch, workflow_dispatch, or something new.
    enforce_runner_timeouts(name, text)

# Automatic publication promotes tested artifacts. It must never sneak the
# heavyweight desktop-stack build back into the release path.
release_artifacts = (wf / 'release-artifacts.yml').read_text()
for required in (
    'git diff --quiet v0.18.47',
    'gh release download v0.18.47',
    'INFILTRATORFS_REQUIRE_OS_INTEGRATION',
    'gh run download',
    'X-InfiltratorFS-Desktop-Integration',
):
    if required not in release_artifacts:
        raise SystemExit(f'release artifact promotion policy missing: {required}')
for forbidden in (
    'apt-get build-dep',
    'bash packaging/build-noble-desktop-integration.sh',
    'timeout-minutes: 12',
):
    if forbidden in release_artifacts:
        raise SystemExit(f'automatic release artifact workflow contains heavyweight path: {forbidden}')

release = (wf / 'release-packages.yml').read_text()
for required in (
    'workflows: ["Build and conformance"]',
    '"Release artifacts"',
    'gh run download',
    'published releases are immutable',
    'git tag --annotate',
):
    if required not in release:
        raise SystemExit(f'release promotion policy missing: {required}')
for forbidden in (
    'apt-get build-dep',
    'build-noble-desktop-integration.sh',
    'cmake --build',
    'timeout-minutes: 12',
):
    if forbidden in release:
        raise SystemExit(f'automatic publisher is rebuilding instead of promoting: {forbidden}')

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
if 'push:' not in on_triggers(kernel):
    raise SystemExit('native kernel workflow automatic trigger unexpectedly removed')

for name in obsolete:
    if (wf / name).exists():
        raise SystemExit(f'obsolete one-shot workflow remains: {name}')

print('CI duration policy: PASS')
