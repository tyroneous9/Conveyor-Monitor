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


def add_range_args(parser):
    parser.add_argument(
        "--healthy-range", nargs=2, metavar=("START", "END"), action="append", default=None,
        help="a time range (unix timestamp or ISO 8601) the belt was known healthy; repeatable for "
             "multiple sessions. Optional if --worn-range is also omitted: windows are then "
             "auto-split in half by time instead",
    )
    parser.add_argument(
        "--worn-range", nargs=2, metavar=("START", "END"), action="append", default=None,
        help="a time range (unix timestamp or ISO 8601) the belt was known worn; repeatable for "
             "multiple sessions. Optional if --healthy-range is also omitted: windows are then "
             "auto-split in half by time instead",
    )


def add_session_args(parser):
    parser.add_argument(
        "--device-id", default=None,
        help="physical device to read from; default: the only device_id present in fft_results (error if there's more than one)",
    )
    add_range_args(parser)


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


def auto_split_ranges(conn, device_id):
    """Fallback for when no --healthy-range/--worn-range is given: split
    every analyzed window for device_id in half by time, earlier half
    labeled healthy and later half worn.

    This is a dev/testing convenience, not a substitute for real recorded
    sessions -- device_id alone can't tell belt condition (see module
    docstring), so this only makes sense for a session where the belt was
    swapped partway through a single recording run."""
    start, end = conn.execute(
        "SELECT MIN(r.received_at), MAX(r.received_at) FROM raw_windows r "
        "JOIN fft_results f ON f.window_id = r.id WHERE r.device_id = ?",
        (device_id,),
    ).fetchone()
    if start is None:
        raise SystemExit(f"no fft_results found for device_id={device_id!r}; can't auto-split by time")
    mid = (start + end) / 2
    return [(start, mid)], [(mid, end)]


def resolve_device_id(conn, table, explicit=None, flag_hint=None):
    """Return `explicit` if given; otherwise auto-detect it as the sole
    distinct device_id in `table`, erroring out if there's more than one
    (ambiguous -- the caller must say which device they mean). `explicit`
    only ever comes from callers that expose a --device-id flag; pass that
    flag's name as `flag_hint` so the ambiguity error tells the user how to
    resolve it (callers without the flag, e.g. generate_figures.py, leave
    it None and rely solely on auto-detection)."""
    if explicit:
        return explicit
    rows = conn.execute(f"SELECT DISTINCT device_id FROM {table}").fetchall()
    if len(rows) != 1:
        ids = ", ".join(r[0] for r in rows) or "(none)"
        suffix = f"; pass {flag_hint} explicitly" if flag_hint else ""
        raise SystemExit(
            f"{table} has {len(rows)} distinct device_id(s) ({ids}), not exactly 1; can't auto-detect which device to use{suffix}"
        )
    return rows[0][0]
