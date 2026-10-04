#!/usr/bin/env python3
"""Compare TouchHost, melonloader and STM32 full-pipeline outputs.

Usage:
    python tests/compare_three_way.py <python.csv> <csharp.csv> <per.csv>

The three files must have the same row ordering. The comparison uses
TouchHost as the source of truth and reports the first differing frame.
"""

from __future__ import annotations

import csv
import sys

REQUIRED_COLUMNS = ("decision",)
DIAGNOSTIC_COLUMNS = (
    "baseline",
    "eff",
    "deriv_eff",
    "deriv2_eff",
    "algo_deriv",
    "algo_deriv2",
    "fsm",
    "large_latch",
)
EXACT = {"decision"}
TOLERANCE = 1e-3
ZONE_DIAGNOSTICS = {"fsm", "large_latch"}


def load(path):
    with open(path, "r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        rows = list(reader)
    return rows


def value(row, column):
    text = row.get(column, "").strip()
    if text == "" or text.lower() == "nan":
        return None
    return int(text) if column in EXACT else float(text)


def normalized(row, column):
    """Read a diagnostic column with the shared 'not applicable' encoding."""
    v = value(row, column)
    if column in ZONE_DIAGNOSTICS and v == -1:
        return None
    return v


def compare(left_name, right_name, left_rows, right_rows):
    limit = min(len(left_rows), len(right_rows))
    first = None

    for index in range(limit):
        left = left_rows[index]
        right = right_rows[index]

        if ((left.get("frame") != right.get("frame")) or
                (left.get("channel") != right.get("channel"))):
            return index, "row alignment"

        for column in REQUIRED_COLUMNS:
            a = normalized(left, column)
            b = normalized(right, column)

            if (a is None) and (b is None):
                continue

            if (a is None) or (b is None):
                return index, column

            same = a == b if column in EXACT else abs(a - b) <= TOLERANCE

            if not same:
                return index, (
                    "%s (%s=%s, %s=%s)"
                    % (column, left_name, a, right_name, b)
                )

    if len(left_rows) != len(right_rows):
        return limit, "row count"

    return None, None


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2

    python_rows = load(sys.argv[1])
    csharp_rows = load(sys.argv[2])
    per_rows = load(sys.argv[3])

    for name, rows in (
        ("python", python_rows),
        ("csharp", csharp_rows),
        ("per", per_rows),
    ):
        print("%s rows: %d" % (name, len(rows)))

    first = None

    for left_name, right_name, left_rows, right_rows in (
        ("python", "csharp", python_rows, csharp_rows),
        ("python", "per", python_rows, per_rows),
        ("csharp", "per", csharp_rows, per_rows),
    ):
        index, reason = compare(left_name, right_name, left_rows, right_rows)
        if reason is None:
            print("MATCH %s vs %s" % (left_name, right_name))
            continue

        print("DIFF %s vs %s at row %d: %s" %
              (left_name, right_name, index, reason))
        if first is None:
            first = (index, reason)

    if first is not None:
        print("first divergence row: %d (%s)" % first)
        return 1

    print("three-way MATCH")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
