#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path

workflow = Path('.github/workflows/temp-buffered-writeback-repair.yml')
text = workflow.read_text()
start_marker = "          python3 - <<'PY'\n"
end_marker = "\n          PY\n"
start = text.index(start_marker) + len(start_marker)
end = text.index(end_marker, start)
lines = text[start:end].splitlines()
code = '\n'.join(line[10:] if line.startswith('          ') else line for line in lines) + '\n'
exec(compile(code, str(workflow) + ':embedded-python', 'exec'), {'__name__': '__main__'})
workflow.unlink()
