#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Release guard for immutable external identities and mounted evidence."""
from pathlib import Path
import re
import sys

root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
sha = re.compile(r"^[0-9a-f]{40}$")
action = re.compile(r"^\s*uses:\s*(?P<repo>[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)(?P<subpath>/[^@\s#]+)?@(?P<ref>[^\s#]+)")
failures = []
for workflow in sorted((root / ".github/workflows").glob("*.yml")):
    for lineno, line in enumerate(workflow.read_text().splitlines(), 1):
        match = action.match(line)
        if match and not sha.fullmatch(match.group("ref")):
            failures.append(f"{workflow}:{lineno}: remote action is not pinned to an immutable commit SHA")
cmake = (root / "CMakeLists.txt").read_text()
lz4 = re.search(r"FetchContent_Declare\(infilfs_lz4(?P<body>.*?)\)\nFetchContent_MakeAvailable", cmake, re.S)
if not lz4 or "GIT_TAG ebb370ca83af193212df4dcbadcc5d87bc0de2f0 # v1.10.0" not in lz4.group("body"):
    failures.append("CMakeLists.txt: retained LZ4 source is not pinned to the qualified commit")
if not lz4 or "GIT_SHALLOW FALSE" not in lz4.group("body"):
    failures.append("CMakeLists.txt: exact LZ4 arbitrary-commit fetch must be non-shallow")
kernel = (root / ".github/workflows/kernel-module.yml").read_text()
if "load/mount test will be skipped" in kernel or "printf 'runtime_kdir=\\n'" in kernel:
    failures.append("kernel-module.yml: mounted qualification still has a skip-success fallback")
if "mounted native qualification cannot proceed" not in kernel:
    failures.append("kernel-module.yml: missing explicit fail-closed running-kernel evidence gate")
if "if: steps.headers.outputs.runtime_kdir" in kernel:
    failures.append("kernel-module.yml: obsolete conditional mounted-test skip guards remain")
if "runtime load/mount skipped" in kernel:
    failures.append("kernel-module.yml: obsolete compile-only success reporting remains")
if failures:
    for failure in failures:
        print(f"dependency identity policy: FAIL: {failure}", file=sys.stderr)
    raise SystemExit(1)
print("Dependency identity and mounted-evidence policy: PASS")
