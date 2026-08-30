#!/usr/bin/env python3
"""Batch FFT analysis: reads raw accel windows from SQLite, computes spectra.

Deliberately separate from ingestion (ingest.py) -- this script never
touches MQTT. It reads windows that don't have a matching fft_results row
yet (storage.fetch_unanalyzed_windows), computes a spectrum per axis
(mean-subtract, Hann window, rfft -- README §4), and writes each result to
the fft_results table.

Run it whenever you want to catch up on analysis -- after a capture
session, by hand, or continuously with --watch. It's idempotent:
already-analyzed windows are skipped, so re-running (or overlapping runs)
is always safe.

Usage:
    pip install -r requirements.txt
    FFT_DB_PATH=<path> python3 analyze_fft.py [--limit N]
    FFT_DB_PATH=<path> python3 analyze_fft.py --watch 30   # loop every 30s
"""

import argparse
import logging
import os
import time

import numpy as np

import storage

# Anchored to this script's own directory, not the process's cwd -- see the
# matching comment in ingest.py. ingest.py and analyze_fft.py must resolve
# to the same file even when launched independently (e.g. one as a service,
# the other from cron) with different working directories.
DEFAULT_DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fft_backend.sqlite3")
DB_PATH = os.environ.get("FFT_DB_PATH", DEFAULT_DB_PATH)
AXES = ("ax", "ay", "az")

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("analyze_fft")


def compute_spectrum(sample_rate_hz, samples):
    n = len(samples)
    windowed = (samples - np.mean(samples)) * np.hanning(n)
    spectrum = np.abs(np.fft.rfft(windowed))
    freq_hz = np.fft.rfftfreq(n, d=1.0 / sample_rate_hz)
    return freq_hz, spectrum


def analyze_window(window):
    sample_rate_hz = window["sample_rate_hz"]
    result = {"sample_rate_hz": sample_rate_hz}
    freq_hz = None
    peak = None  # (axis, freq_hz, amplitude)
    for axis in AXES:
        samples = np.asarray(window[axis], dtype=np.float64)
        freq_hz, spectrum = compute_spectrum(sample_rate_hz, samples)
        result[f"fft_{axis}"] = spectrum.tolist()
        peak_idx = int(np.argmax(spectrum[1:])) + 1  # skip the DC bin
        if peak is None or spectrum[peak_idx] > peak[2]:
            peak = (axis, freq_hz[peak_idx], spectrum[peak_idx])
    result["freq_hz"] = freq_hz.tolist()
    return result, peak


def run_once(conn, limit):
    windows = storage.fetch_unanalyzed_windows(conn, limit=limit)
    log.info("found %d unanalyzed window(s) in %s", len(windows), DB_PATH)

    for window in windows:
        result, peak = analyze_window(window)
        storage.store_fft_result(conn, window["window_id"], window["device_id"], result, peak)
        log.info(
            "window_id=%d device=%s peak=%.1fHz (%s) amp=%.3f",
            window["window_id"], window["device_id"], peak[1], peak[0], peak[2],
        )

    return len(windows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--limit", type=int, default=100, help="max windows to process per run")
    parser.add_argument(
        "--watch", type=float, default=None, metavar="SECONDS",
        help="keep running, checking for new windows every SECONDS instead of exiting after one pass",
    )
    args = parser.parse_args()

    conn = storage.connect(DB_PATH)

    if args.watch is None:
        run_once(conn, args.limit)
        log.info("done")
        return

    log.info("watching %s every %.0fs (Ctrl+C to stop)", DB_PATH, args.watch)
    while True:
        run_once(conn, args.limit)
        time.sleep(args.watch)


if __name__ == "__main__":
    main()
