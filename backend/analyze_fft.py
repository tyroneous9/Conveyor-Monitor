#!/usr/bin/env python3
"""Batch FFT analysis: reads raw accel windows from SQLite, computes FFT.

Reads windows that don't have a matching fft_results row yet and writes the analysis to fft_results.

Usage:
    FFT_DB_PATH=<path> python3 analyze_fft.py [--limit N]
    FFT_DB_PATH=fft_backend.sqlite3 python3 analyze_fft.py
"""

import argparse
import logging
import os

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
    """Input one axis' samples and output its frequency spectrum (frequency AND magnitude). 
    The Hann window smooths the edges of the windows."""
    n_samples = len(samples)
    windowed = (samples - np.mean(samples)) * np.hanning(n_samples)
    spectrum = np.abs(np.fft.rfft(windowed))
    freq_hz = np.fft.rfftfreq(n_samples, d=1.0 / sample_rate_hz)
    return freq_hz, spectrum


def analyze_window(window):
    """Compute a spectrum for each axis of one raw window and find the
    single largest peak across all three (skipping each axis's DC bin).
    Returns (result dict ready for storage.store_fft_result, peak tuple)."""
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
    args = parser.parse_args()

    conn = storage.connect(DB_PATH)
    run_once(conn, args.limit)
    log.info("done")


if __name__ == "__main__":
    main()
