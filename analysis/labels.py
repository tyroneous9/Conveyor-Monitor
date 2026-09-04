"""Turns operator-recorded recording sessions into per-window healthy/worn
labels, shared by classify_faults.py and generate_figures.py.

One physical device gets moved between a known-healthy and a known-worn
belt on different runs -- device_id (the ESP32's MAC address) carries no
information about belt condition, so ground truth comes from which
recorded time range a window's timestamp falls in instead.
"""

from datetime import datetime


def parse_ts(s):
    """A unix timestamp or an ISO 8601 string, either works."""
    try:
        return float(s)
    except ValueError:
        return datetime.fromisoformat(s).timestamp()


def add_session_args(parser):
    parser.add_argument(
        "--device-id", default=None,
        help="physical device to read from; default: the only device_id present in fft_results (error if there's more than one)",
    )
    parser.add_argument(
        "--healthy-range", nargs=2, metavar=("START", "END"), action="append", default=None,
        help="a time range (unix timestamp or ISO 8601) the belt was known healthy; repeatable for multiple sessions",
    )
    parser.add_argument(
        "--worn-range", nargs=2, metavar=("START", "END"), action="append", default=None,
        help="a time range (unix timestamp or ISO 8601) the belt was known worn; repeatable for multiple sessions",
    )


def parse_ranges(raw_ranges):
    if not raw_ranges:
        return []
    return [(parse_ts(a), parse_ts(b)) for a, b in raw_ranges]


def label_for(received_at, healthy_ranges, worn_ranges):
    """"healthy"/"worn"/None depending on which set of ranges (if any) this
    window's timestamp falls inside."""
    if any(start <= received_at <= end for start, end in healthy_ranges):
        return "healthy"
    if any(start <= received_at <= end for start, end in worn_ranges):
        return "worn"
    return None


def resolve_device_id(conn, table, explicit):
    """Return `explicit` if given; otherwise auto-detect it as the sole
    distinct device_id in `table`, erroring out if there's more than one
    (ambiguous -- the caller must say which device they mean)."""
    if explicit:
        return explicit
    rows = conn.execute(f"SELECT DISTINCT device_id FROM {table}").fetchall()
    if len(rows) != 1:
        ids = ", ".join(r[0] for r in rows) or "(none)"
        raise SystemExit(
            f"--device-id not given and {table} has {len(rows)} distinct device_id(s) ({ids}); pass --device-id explicitly"
        )
    return rows[0][0]
