#!/usr/bin/env python3
"""Batch FFT analysis: reads raw accel windows from SQLite, computes spectra.

Deliberately separate from ingestion (ingest.py) -- this script never
touches MQTT. It reads windows that don't have a matching fft_results row
yet (storage.fetch_unanalyzed_windows), computes a spectrum per axis
(mean-subtract, Hann window, rfft -- README §4), and writes each result to
the fft_results table.

Run it whenever you want to catch up on analysis -- after a capture
session, or on a cron schedule. It's idempotent: already-analyzed windows
are skipped, so re-running is always safe.

Usage:
    pip install -r requirements.txt
    FFT_DB_PATH=<path> python3 analyze_fft.py [--limit N]
"""

import argparse
import logging
import os

import numpy as np

import storage

DB_PATH = os.environ.get("FFT_DB_PATH", "fft_backend.sqlite3")
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--limit", type=int, default=100, help="max windows to process per run")
    args = parser.parse_args()

    conn = storage.connect(DB_PATH)
    windows = storage.fetch_unanalyzed_windows(conn, limit=args.limit)
    log.info("found %d unanalyzed window(s) in %s", len(windows), DB_PATH)

    for window in windows:
        result, peak = analyze_window(window)
        storage.store_fft_result(conn, window["window_id"], window["device_id"], result, peak)
        log.info(
            "window_id=%d device=%s peak=%.1fHz (%s) amp=%.3f",
            window["window_id"], window["device_id"], peak[1], peak[0], peak[2],
        )

    log.info("done")


if __name__ == "__main__":
    main()
