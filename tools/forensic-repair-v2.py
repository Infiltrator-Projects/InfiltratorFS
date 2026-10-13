#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path

# Normalize the two prose examples to the sentence form consumed by the
# reviewed repair script. This keeps the repair exact and fail-closed rather
# than weakening it into broad regular-expression replacement.
path = Path('docs/IAC1-2026-HARDWARE-DESIGN.md')
text = path.read_text()
old = ('A 4-logical-CPU machine has a filesystem budget of 3; a\n'
       '64-logical-CPU machine has a filesystem budget of 63.')
new = ('On a 4-logical-CPU machine, up to 3 CPU-bound filesystem worker units may run concurrently.\n'
       'On a 64-logical-CPU machine, up to 63 may run concurrently.')
if text.count(old) != 1:
    raise SystemExit('IAC1 hardware design: logical-CPU example block changed unexpectedly')
path.write_text(text.replace(old, new, 1))

script = Path('tools/forensic-repair.py')
exec(compile(script.read_text(), str(script), 'exec'))
