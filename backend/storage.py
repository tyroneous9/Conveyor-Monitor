"""SQLite storage for raw accelerometer windows and their derived FFT spectra.

SQLite, not a client/server DB (Postgres, etc.): writes come from one
process at a time (ingest.py, then separately analyze_fft.py), write volume
is one row per window, and a single file needs no separate daemon competing
with Mosquitto + these scripts for the Pi's resources. WAL mode is enabled
so /analysis notebooks can read the file concurrently with either writer.

raw_windows and fft_results are deliberately separate tables written by
separate scripts (ingest.py, analyze_fft.py) -- see those files.
"""

import json
import sqlite3
import time

SCHEMA = """
CREATE TABLE IF NOT EXISTS raw_windows (
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
    window_id INTEGER NOT NULL REFERENCES raw_windows(id),
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

CREATE INDEX IF NOT EXISTS idx_raw_windows_device_time
    ON raw_windows(device_id, received_at);
CREATE INDEX IF NOT EXISTS idx_fft_results_device_time
    ON fft_results(device_id, analyzed_at);
CREATE INDEX IF NOT EXISTS idx_fft_results_window
    ON fft_results(window_id);
"""


def connect(db_path):
    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.executescript(SCHEMA)
    conn.commit()
    return conn


def store_window(conn, device_id, payload):
    """Write one raw accel window as-is. Returns the new row's id."""
    cur = conn.execute(
        "INSERT INTO raw_windows (device_id, received_at, sample_rate_hz, ax, ay, az) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        (
            device_id,
            time.time(),
            payload["sample_rate_hz"],
            json.dumps(payload["ax"]),
            json.dumps(payload["ay"]),
            json.dumps(payload["az"]),
        ),
    )
    conn.commit()
    return cur.lastrowid


def fetch_unanalyzed_windows(conn, limit=100):
    """Raw windows that don't have a matching fft_results row yet."""
    cur = conn.execute(
        "SELECT r.id, r.device_id, r.sample_rate_hz, r.ax, r.ay, r.az "
        "FROM raw_windows r "
        "LEFT JOIN fft_results f ON f.window_id = r.id "
        "WHERE f.id IS NULL "
        "ORDER BY r.id "
        "LIMIT ?",
        (limit,),
    )
    return [
        {
            "window_id": row[0],
            "device_id": row[1],
            "sample_rate_hz": row[2],
            "ax": json.loads(row[3]),
            "ay": json.loads(row[4]),
            "az": json.loads(row[5]),
        }
        for row in cur.fetchall()
    ]


def store_fft_result(conn, window_id, device_id, result, peak):
    axis, peak_freq_hz, peak_amp = peak
    conn.execute(
        "INSERT INTO fft_results "
        "(window_id, device_id, analyzed_at, sample_rate_hz, freq_hz, "
        " fft_ax, fft_ay, fft_az, peak_axis, peak_freq_hz, peak_amp) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        (
            window_id,
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
