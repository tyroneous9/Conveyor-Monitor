#!/usr/bin/env python3
"""Synthetic raw-window generator, for exercising analyze_fft.py and
analysis/classify_faults.py without real ESP32 + MPU6050 hardware.

Modeled on the two vibration sources documented in the README's "Analysis
results": motor rotation (~29.3Hz, present at similar amplitude regardless
of belt condition) and belt-pass frequency (~8-9Hz, negligible on a
healthy belt and the dominant peak on a worn one). This is a qualitative
model, not a calibrated match to the README's specific numbers -- every
window draws its own amplitudes, frequencies, and noise from randomized
ranges (rather than one fixed value repeated every window), and the noise
floor is high enough that healthy and worn windows aren't always cleanly
separable, the way real sensor data wouldn't be either.

Windows are written straight to raw_windows via storage.store_window, the
same entrypoint ingest.py uses, so downstream scripts can't tell them from
real hardware data.

Usage:
    python3 window_gen.py --device-id sim-01 --condition healthy --count 200
    python3 window_gen.py --device-id sim-01 --condition worn --count 200

Each invocation's windows are timestamped at generation time (like real
ingestion), so running healthy then worn as separate invocations produces
two naturally-separated time ranges -- suitable for classify_faults.py's
--healthy-range/--worn-range.
"""

import argparse
import logging
import os

import numpy as np

import storage

# Anchored to this script's own directory -- see the matching comment in
# ingest.py/analyze_fft.py.
DEFAULT_DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fft_backend.sqlite3")
DB_PATH = os.environ.get("FFT_DB_PATH", DEFAULT_DB_PATH)

SAMPLE_RATE_HZ = 500.0
WINDOW_SAMPLES = 256  # matches the firmware's window size (README §1)

MOTOR_FREQ_HZ = 29.3
MOTOR_FREQ_JITTER_HZ = 1.0
MOTOR_AMPLITUDE_RANGE_G = (0.015, 0.06)

BELT_FREQ_HZ = 8.7
BELT_FREQ_JITTER_HZ = 1.2
BELT_AMPLITUDE_HEALTHY_RANGE_G = (0.0, 0.03)  # stays buried under the noise floor most of the time
BELT_AMPLITUDE_WORN_RANGE_G = (0.12, 0.5)  # wide range: wear severity varies
BELT_HARMONIC_RATIO_RANGE = (0.1, 0.4)  # a worn belt vibrates impulsively, not as a pure sinusoid

# High enough, relative to MOTOR_AMPLITUDE_RANGE_G, that a noise spike can
# occasionally outweigh the real peak on a healthy window -- real sensor
# data doesn't separate as cleanly as a hand-picked constant would.
NOISE_STD_G = 0.05
GRAVITY_G = 1.0  # az carries the ~1g gravity offset (mounted roughly upright)

# ay is the axis classify_faults.py reads (storage.fetch_fft_results); ax/az
# get the same underlying vibration at reduced gain, as weaker structural
# cross-coupling would look on a real mount.
AXIS_GAIN = {"ax": 0.35, "ay": 1.0, "az": 0.25}

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("window_gen")


def generate_window(condition, rng):
    """One synthetic raw window (dict ready for storage.store_window):
    motor-rotation + belt-pass sinusoids (with a harmonic on the belt-pass
    term) at per-axis gain, plus gaussian noise and az's gravity offset.
    Every amplitude and frequency is redrawn per window, so no two windows
    (even of the same condition) are identical."""
    t = np.arange(WINDOW_SAMPLES) / SAMPLE_RATE_HZ

    motor_freq = MOTOR_FREQ_HZ + rng.normal(0, MOTOR_FREQ_JITTER_HZ)
    belt_freq = BELT_FREQ_HZ + rng.normal(0, BELT_FREQ_JITTER_HZ)
    motor_amplitude = rng.uniform(*MOTOR_AMPLITUDE_RANGE_G)
    belt_range = BELT_AMPLITUDE_WORN_RANGE_G if condition == "worn" else BELT_AMPLITUDE_HEALTHY_RANGE_G
    belt_amplitude = rng.uniform(*belt_range)
    harmonic_ratio = rng.uniform(*BELT_HARMONIC_RATIO_RANGE)

    vibration = (
        motor_amplitude * np.sin(2 * np.pi * motor_freq * t + rng.uniform(0, 2 * np.pi))
        + belt_amplitude * np.sin(2 * np.pi * belt_freq * t + rng.uniform(0, 2 * np.pi))
        + belt_amplitude * harmonic_ratio * np.sin(2 * np.pi * 2 * belt_freq * t + rng.uniform(0, 2 * np.pi))
    )

    window = {"sample_rate_hz": SAMPLE_RATE_HZ}
    for axis, gain in AXIS_GAIN.items():
        offset = GRAVITY_G if axis == "az" else 0.0
        noise = rng.normal(0, NOISE_STD_G, WINDOW_SAMPLES)
        window[axis] = (offset + gain * vibration + noise).tolist()
    return window


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device-id", required=True, help="device_id to tag generated windows with")
    parser.add_argument("--condition", choices=("healthy", "worn"), required=True,
                         help="which belt condition to simulate")
    parser.add_argument("--count", type=int, required=True, help="number of windows to generate")
    parser.add_argument("--seed", type=int, default=None, help="RNG seed, for reproducible output")
    args = parser.parse_args()

    rng = np.random.default_rng(args.seed)
    conn = storage.connect(DB_PATH)

    for _ in range(args.count):
        window = generate_window(args.condition, rng)
        window_id = storage.store_window(conn, args.device_id, window)
        log.info("device=%s condition=%s stored window_id=%d", args.device_id, args.condition, window_id)

    log.info("wrote %d %s window(s) for device=%s to %s", args.count, args.condition, args.device_id, DB_PATH)


if __name__ == "__main__":
    main()
