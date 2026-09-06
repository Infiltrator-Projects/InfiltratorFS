#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# One-shot validated source repair; remove after the repair commit lands.
from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text()
    if text.count(old) != 1:
        raise SystemExit(f"{path}: expected exactly one replacement target, found {text.count(old)}")
    path.write_text(text.replace(old, new, 1))


readme = Path("README.md")
text = readme.read_text()
text = text.replace("**Current source version:** 0.18.43 (Format 0.18)<br>",
                    "**Current source version:** 0.18.44 (Format 0.18)<br>")
text = text.replace("**Current source:** 0.18.43  ",
                    "**Current source:** 0.18.44  ")
readme.write_text(text)

workflow = Path(".github/workflows/release-packages.yml")
old = '''      - name: Require native kernel qualification for the same commit
        timeout-minutes: 35
        env:
          EXPECTED_SHA: ${{ github.event.workflow_run.head_sha }}
          GH_TOKEN: ${{ github.token }}
        shell: bash
        run: |
          set -euo pipefail
          api="repos/${GITHUB_REPOSITORY}/actions/runs?head_sha=${EXPECTED_SHA}&event=push&per_page=20"
          for attempt in $(seq 1 180); do
            mapfile -t native < <(
              gh api "$api" --jq '\n                first(.workflow_runs[] |\n                  select(.name == "Native Linux kernel module")) |\n                .status, (.conclusion // ""), .html_url'
            )
            status="${native[0]:-}"
            conclusion="${native[1]:-}"
            run_url="${native[2]:-}"
            if [[ "$status" == completed ]]; then
              if [[ "$conclusion" != success ]]; then
                printf 'Native qualification failed for %s: %s (%s)\\n' \\
                  "$EXPECTED_SHA" "$conclusion" "$run_url" >&2
                exit 1
              fi
              printf 'Native qualification passed for %s: %s\\n' \\
                "$EXPECTED_SHA" "$run_url"
              exit 0
            fi
            sleep 10
          done
          printf 'Timed out waiting for native qualification of %s.\\n' \\
            "$EXPECTED_SHA" >&2
          exit 1

      - name: Record heavy qualification policy
        shell: bash
        run: |
          set -euo pipefail
          echo "Heavy filesystem qualification is weekly/manual milestone evidence and is not an automatic per-release prerequisite."
'''
new = '''      - name: Require Linux release qualifications for the same commit
        timeout-minutes: 70
        env:
          EXPECTED_SHA: ${{ github.event.workflow_run.head_sha }}
          GH_TOKEN: ${{ github.token }}
        shell: bash
        run: |
          set -euo pipefail
          api="repos/${GITHUB_REPOSITORY}/actions/runs?head_sha=${EXPECTED_SHA}&event=push&per_page=50"
          required=(
            "Native Linux kernel module"
            "Linux root-volume qualification"
            "Linux root boot qualification"
          )
          for attempt in $(seq 1 420); do
            all_complete=1
            for workflow in "${required[@]}"; do
              mapfile -t state < <(
                gh api "$api" --jq ".workflow_runs | map(select(.name == \\\"$workflow\\\")) | first | .status, (.conclusion // \\\"\\\"), .html_url"
              )
              status="${state[0]:-}"
              conclusion="${state[1]:-}"
              run_url="${state[2]:-}"
              if [[ "$status" != completed ]]; then
                all_complete=0
                continue
              fi
              if [[ "$conclusion" != success ]]; then
                printf '%s failed for %s: %s (%s)\\n' \\
                  "$workflow" "$EXPECTED_SHA" "$conclusion" "$run_url" >&2
                exit 1
              fi
            done
            if [[ "$all_complete" == 1 ]]; then
              printf 'Native Linux, root-volume and root-boot qualification passed for %s.\\n' \\
                "$EXPECTED_SHA"
              exit 0
            fi
            sleep 10
          done
          printf 'Timed out waiting for Linux release qualifications of %s.\\n' \\
            "$EXPECTED_SHA" >&2
          exit 1
'''
replace_once(workflow, old, new)
