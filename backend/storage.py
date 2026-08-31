"""SQLite storage for raw accelerometer captures and their derived FFT spectra.

SQLite, not a client/server DB (Postgres, etc.): writes come from one
process at a time (ingest.py, then separately analyze_fft.py), write volume
is one row per capture, and a single file needs no separate daemon competing
with Mosquitto + these scripts for the Pi's resources. WAL mode is enabled
so /analysis notebooks can read the file concurrently with either writer.

raw_captures and fft_results are deliberately separate tables written by
separate scripts (ingest.py, analyze_fft.py) -- see those files.
"""

import json
import sqlite3
import time

SCHEMA = """
CREATE TABLE IF NOT EXISTS raw_captures (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL,
    received_at REAL NOT NULL,
    sample_rate_hz REAL NOT NULL,
    ax TEXT NOT NULL,
    ay TEXT NOT NULL,
    az TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS fft_results (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    capture_id INTEGER NOT NULL REFERENCES raw_captures(id),
    device_id TEXT NOT NULL,
    analyzed_at REAL NOT NULL,
    sample_rate_hz REAL NOT NULL,
    freq_hz TEXT NOT NULL,
    fft_ax TEXT NOT NULL,
    fft_ay TEXT NOT NULL,
    fft_az TEXT NOT NULL,
    peak_axis TEXT NOT NULL,
    peak_freq_hz REAL NOT NULL,
    peak_amp REAL NOT NULL
);

CREATE TABLE IF NOT EXISTS baselines (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL,
    feature TEXT NOT NULL,
    mean REAL NOT NULL,
    std REAL NOT NULL,
    n_captures INTEGER NOT NULL,
    computed_at REAL NOT NULL
);

CREATE TABLE IF NOT EXISTS classifications (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    capture_id INTEGER NOT NULL REFERENCES raw_captures(id),
    device_id TEXT NOT NULL,
    feature_value REAL NOT NULL,
    threshold REAL NOT NULL,
    predicted_label TEXT NOT NULL,
    classified_at REAL NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_raw_captures_device_time
    ON raw_captures(device_id, received_at);
CREATE INDEX IF NOT EXISTS idx_fft_results_device_time
    ON fft_results(device_id, analyzed_at);
CREATE INDEX IF NOT EXISTS idx_fft_results_capture
    ON fft_results(capture_id);
CREATE INDEX IF NOT EXISTS idx_classifications_capture
    ON classifications(capture_id);
"""


def connect(db_path):
    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.executescript(SCHEMA)
    conn.commit()
    return conn


def store_capture(conn, device_id, payload, received_at=None):
    """Write one raw accel capture as-is. Returns the new row's id.

    received_at defaults to now; callers backdating a session (see
    backend/seed_samples.py) can override it.
    """
    cur = conn.execute(
        "INSERT INTO raw_captures (device_id, received_at, sample_rate_hz, ax, ay, az) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        (
            device_id,
            received_at if received_at is not None else time.time(),
            payload["sample_rate_hz"],
            json.dumps(payload["ax"]),
            json.dumps(payload["ay"]),
            json.dumps(payload["az"]),
        ),
    )
    conn.commit()
    return cur.lastrowid


def fetch_unanalyzed_captures(conn, limit=100):
    """Raw captures that don't have a matching fft_results row yet."""
    cur = conn.execute(
        "SELECT r.id, r.device_id, r.sample_rate_hz, r.ax, r.ay, r.az "
        "FROM raw_captures r "
        "LEFT JOIN fft_results f ON f.capture_id = r.id "
        "WHERE f.id IS NULL "
        "ORDER BY r.id "
        "LIMIT ?",
        (limit,),
    )
    return [
        {
            "capture_id": row[0],
            "device_id": row[1],
            "sample_rate_hz": row[2],
            "ax": json.loads(row[3]),
            "ay": json.loads(row[4]),
            "az": json.loads(row[5]),
        }
        for row in cur.fetchall()
    ]


def store_fft_result(conn, capture_id, device_id, result, peak):
    axis, peak_freq_hz, peak_amp = peak
    conn.execute(
        "INSERT INTO fft_results "
        "(capture_id, device_id, analyzed_at, sample_rate_hz, freq_hz, "
        " fft_ax, fft_ay, fft_az, peak_axis, peak_freq_hz, peak_amp) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        (
            capture_id,
            device_id,
            time.time(),
            result["sample_rate_hz"],
            json.dumps(result["freq_hz"]),
            json.dumps(result["fft_ax"]),
            json.dumps(result["fft_ay"]),
            json.dumps(result["fft_az"]),
            axis,
            peak_freq_hz,
            peak_amp,
        ),
    )
    conn.commit()


def store_baseline(conn, device_id, feature, mean, std, n_captures):
    """Persist a computed baseline as a durable, inspectable artifact --
    not a number silently recomputed inside a script every run. Callers
    (see analysis/classify_faults.py) can recompute and re-store to update
    it; each call adds a new row rather than overwriting, so history isn't
    lost."""
    conn.execute(
        "INSERT INTO baselines (device_id, feature, mean, std, n_captures, computed_at) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        (device_id, feature, mean, std, n_captures, time.time()),
    )
    conn.commit()


def fetch_latest_baseline(conn, device_id, feature):
    row = conn.execute(
        "SELECT mean, std, n_captures, computed_at FROM baselines "
        "WHERE device_id = ? AND feature = ? ORDER BY computed_at DESC LIMIT 1",
        (device_id, feature),
    ).fetchone()
    if row is None:
        return None
    return {"mean": row[0], "std": row[1], "n_captures": row[2], "computed_at": row[3]}


def store_classification(conn, capture_id, device_id, feature_value, threshold, predicted_label):
    conn.execute(
        "INSERT INTO classifications "
        "(capture_id, device_id, feature_value, threshold, predicted_label, classified_at) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        (capture_id, device_id, feature_value, threshold, predicted_label, time.time()),
    )
    conn.commit()


def fetch_fft_results(conn, device_id):
    """All fft_results rows for a device, ordered by capture id -- used by
    the fault classifier to compute per-capture features (and label them by
    capture time -- see analysis/labels.py) from the already-stored
    spectrum, without recomputing the FFT."""
    rows = conn.execute(
        "SELECT r.id, r.received_at, f.sample_rate_hz, f.freq_hz, f.fft_ay, f.peak_freq_hz, f.peak_amp "
        "FROM raw_captures r JOIN fft_results f ON f.capture_id = r.id "
        "WHERE r.device_id = ? ORDER BY r.id",
        (device_id,),
    ).fetchall()
    return [
        {
            "capture_id": row[0],
            "received_at": row[1],
            "sample_rate_hz": row[2],
            "freq_hz": json.loads(row[3]),
            "fft_ay": json.loads(row[4]),
            "peak_freq_hz": row[5],
            "peak_amp": row[6],
        }
        for row in rows
    ]
