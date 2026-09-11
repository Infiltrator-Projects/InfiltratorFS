#!/usr/bin/env python3
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
wf = root / '.github' / 'workflows'

manual_long = {
    'heavy-qualification.yml',
    'root-boot-qualification.yml',
    'desktop-formatter-qualification.yml',
}
automatic = {
    'ci.yml',
    'linux-metadata-qualification.yml',
    'resize-qualification.yml',
    'root-volume-qualification.yml',
    'windows-bridge.yml',
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
}

for name in manual_long:
    text = (wf / name).read_text()
    on_block = text.split('on:\n', 1)[1].split('\npermissions:', 1)[0]
    if 'workflow_dispatch:' not in on_block:
        raise SystemExit(f'{name}: manual dispatch missing')
    for trigger in ('push:', 'schedule:', 'pull_request:', 'workflow_run:'):
        if trigger in on_block:
            raise SystemExit(f'{name}: long qualification has automatic trigger {trigger}')

for name in automatic:
    text = (wf / name).read_text()
    lines = text.splitlines()
    jobs = lines.index('jobs:')
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
            raise SystemExit(f'{name}:{job}: automatic job has no timeout')
        minutes = int(timeout.split(':', 1)[1].strip())
        if minutes > 10:
            raise SystemExit(f'{name}:{job}: automatic timeout is {minutes} minutes')

for name in obsolete:
    if (wf / name).exists():
        raise SystemExit(f'obsolete one-shot workflow remains: {name}')

print('CI duration policy: PASS')
