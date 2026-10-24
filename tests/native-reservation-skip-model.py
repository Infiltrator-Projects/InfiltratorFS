#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Allocation-run skip proof model: verify search identity at all small states.

The oracle is the former linear candidate search. This is not a substitute for
the live kernel reservation-concurrency test, which remains in CI; it proves
that skipping a blocker cannot skip the earliest valid candidate under a
stable two-pass bitmap, including the non-wrapping sweep boundary.
"""

from itertools import product


def reference(occupied, count, hint):
    total = len(occupied)
    for lo, hi in ((hint, total), (0, hint)):
        for candidate in range(lo, hi - count + 1):
            if not any(occupied[candidate:candidate + count]):
                return candidate
    return None


def skip_blocker(occupied, count, hint):
    total = len(occupied)
    for lo, hi in ((hint, total), (0, hint)):
        candidate = lo
        while candidate <= hi - count:
            busy = next((b for b in range(candidate, candidate + count)
                         if occupied[b]), candidate + count)
            if busy < candidate + count:
                candidate = busy + 1
                continue
            return candidate
    return None


def main():
    cases = 0
    for n in range(1, 12):
        for occupied in product((False, True), repeat=n):
            for count in range(1, n + 1):
                for hint in range(n + 1):
                    expected = reference(occupied, count, hint)
                    found = skip_blocker(occupied, count, hint)
                    if expected != found:
                        raise AssertionError(
                            (n, occupied, count, hint, expected, found))
                    cases += 1
    print(f"Native reservation skip model: PASS ({cases:,} states)")


if __name__ == "__main__":
    main()
