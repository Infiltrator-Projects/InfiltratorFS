#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Compatibility entry point retained for existing documentation, CI safety
# checks and operators. The maintained destructive qualification lives in the
# adjacent script.
set -Eeuo pipefail

# Keep these explicit constants here as a second safety contract: CI verifies
# that the public entry point is still hard-wired to the dedicated test target.
TARGET="/dev/mmcblk0p22"
EXPECTED_PARTNO="22"
readonly TARGET EXPECTED_PARTNO

exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/native-partition22-qualification.sh" "$@"
