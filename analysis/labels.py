#!/usr/bin/env python3
"""Label recorded windows healthy/worn from operator-recorded session time
ranges, storing the result in the `window_labels` table (backend/storage.py)
so analysis/classify_faults.py and analysis/generate_figures.py can read a
window's ground truth without knowing anything about recording sessions.

The one physical device gets moved between a known-healthy and a
known-worn belt on different runs, so ground truth comes from which
recorded time range a window's timestamp falls in, not from the data
itself.

Run this once after recording a healthy and a worn session, and again
whenever a new session is recorded or a range needs correcting -- each run
clears every previously stored label and relabels from scratch using
exactly the ranges given, so the table never ends up a stale mix of old
and new sessions.

Usage:
    python3 labels.py \\
        --healthy-range 2026-08-20T09:00 2026-08-20T11:00 \\
        --worn-range 2026-08-22T09:00 2026-08-22T11:00
"""

import argparse
import os
import sys
from datetime import datetime

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "backend"))
import storage  # noqa: E402

DEFAULT_DB_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "backend", "fft_db.sqlite3"
)
DB_PATH = os.environ.get("FFT_DB_PATH", DEFAULT_DB_PATH)


def parse_ts(s):
    """A unix timestamp or an ISO 8601 string, either works."""
    try:
        return float(s)
    except ValueError:
        return datetime.fromisoformat(s).timestamp()


def parse_ranges(raw_ranges):
    if not raw_ranges:
        return []
    return [(parse_ts(a), parse_ts(b)) for a, b in raw_ranges]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--healthy-range", nargs=2, metavar=("START", "END"), action="append", required=True,
        help="a time range (unix timestamp or ISO 8601) the belt was known healthy; repeatable for "
             "multiple sessions",
    )
    parser.add_argument(
        "--worn-range", nargs=2, metavar=("START", "END"), action="append", required=True,
        help="a time range (unix timestamp or ISO 8601) the belt was known worn; repeatable for "
             "multiple sessions",
    )
    args = parser.parse_args()

    conn = storage.connect(DB_PATH)

    healthy_ranges = parse_ranges(args.healthy_range)
    worn_ranges = parse_ranges(args.worn_range)

    storage.clear_labels(conn)
    n_healthy = storage.label_windows(conn, healthy_ranges, "healthy")
    n_worn = storage.label_windows(conn, worn_ranges, "worn")
    if not n_healthy:
        raise SystemExit("no windows fall inside --healthy-range")
    if not n_worn:
        raise SystemExit("no windows fall inside --worn-range")

    print(f"labeled {n_healthy} window(s) healthy, {n_worn} window(s) worn")


if __name__ == "__main__":
    main()
