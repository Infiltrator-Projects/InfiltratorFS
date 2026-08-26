#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Compatibility entry point retained for existing documentation and operators.
# The maintained destructive partition-22 qualification lives in the adjacent
# script so its safety/parser logic can evolve without duplicating test code.
set -Eeuo pipefail
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/native-partition22-qualification.sh" "$@"
