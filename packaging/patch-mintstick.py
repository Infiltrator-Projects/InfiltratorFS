#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Retired Mintstick integration guard.

Linux Mint's Mintstick/Nemo "USB Stick Formatter" is a whole-device formatter:
it replaces the target drive's partition table and creates one new partition.
It is therefore not a safe integration point for formatting an existing
InfiltratorFS partition. Older InfiltratorFS packages patched Mintstick; current
packages actively restore the distribution's stock Mintstick files instead.

This file intentionally fails closed if invoked so the retired integration
cannot be accidentally re-enabled from an old build script or local checkout.
"""

import sys

sys.stderr.write(
    "InfiltratorFS: Mintstick integration is disabled because Mintstick "
    "repartitions whole devices. Use InfiltratorFS Manager or the "
    "libblockdev/GNOME Disks partition formatter instead.\n"
)
raise SystemExit(2)
