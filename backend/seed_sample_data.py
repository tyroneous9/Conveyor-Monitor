#!/usr/bin/env python3
"""Generate physically plausible fake accelerometer windows for testing.

Dev/test utility, not part of the production ingest -> analyze path: it
writes straight into raw_windows via the same storage.store_window()
ingest.py uses, so the data enters the system exactly the way a real
device's would and analyze_fft.py / the notebook need no special-casing to
consume it. It never touches fft_results or invokes analyze_fft.py itself --
run that separately afterward.

Signal model per window (see PLAN in git history / TODO.md for the full
reasoning): gravity DC offset (mostly on az) + a motor-rotation fundamental
and 2nd harmonic (present in both conditions -- that's not what a worn belt
changes) + a belt-pass fundamental and harmonics, whose amplitude is what
actually differs: small when healthy, boosted ~6-8x with stronger harmonics
when worn (README §4's description of what a worn belt does to a spectrum).
Small per-window jitter and a Gaussian noise floor keep consecutive windows
from being identical repeats.

Usage:
    pip install -r requirements.txt
    python3 seed_fake_data.py --condition healthy --num-windows 20
    python3 seed_fake_data.py --condition worn --num-windows 20
"""

import argparse
import logging
import os
import time

import numpy as np

import storage

DEFAULT_DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fft_backend.sqlite3")
DB_PATH = os.environ.get("FFT_DB_PATH", DEFAULT_DB_PATH)

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("seed_fake_data")


def jitter(rng, base, spread=0.10):
    return base * (1 + rng.uniform(-spread, spread))


def sine_sum(t, harmonics, phase=0.0):
    total = np.zeros_like(t)
    for freq, amp in harmonics:
        total += amp * np.sin(2 * np.pi * freq * t + phase)
    return total


def generate_window(rng, condition, sample_rate_hz, window_size, motor_freq_hz, belt_freq_hz):
    t = np.arange(window_size) / sample_rate_hz

    motor_freq = jitter(rng, motor_freq_hz, spread=0.02)
    belt_freq = jitter(rng, belt_freq_hz, spread=0.02)

    motor_amp = jitter(rng, 0.05)
    motor_2h_amp = jitter(rng, 0.01)

    if condition == "worn":
        belt_amp = jitter(rng, 0.35)
        belt_2h_amp = jitter(rng, 0.12)
        belt_3h_amp = jitter(rng, 0.05)
    else:
        # Clearly subordinate to motor_amp: a healthy belt should barely
        # register at its pass frequency, so the motor fundamental stays
        # the dominant peak (see the note on this bug in TODO.md).
        belt_amp = jitter(rng, 0.015)
        belt_2h_amp = jitter(rng, 0.004)
        belt_3h_amp = jitter(rng, 0.002)

    harmonics = [
        (motor_freq, motor_amp),
        (2 * motor_freq, motor_2h_amp),
        (belt_freq, belt_amp),
        (2 * belt_freq, belt_2h_amp),
        (3 * belt_freq, belt_3h_amp),
    ]

    noise_std = 0.008
    vib_ax = sine_sum(t, harmonics, phase=0.0)
    vib_ay = sine_sum(t, harmonics, phase=np.pi / 4)  # transverse axis, phase-shifted

    ax = 0.05 + vib_ax + rng.normal(0, noise_std, window_size)
    ay = -0.03 + vib_ay + rng.normal(0, noise_std, window_size)
    az = 0.98 + rng.normal(0, noise_std * 0.5, window_size)  # axial: mostly gravity, little vibration

    return ax.tolist(), ay.tolist(), az.tolist()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--condition", choices=("healthy", "worn"), required=True)
    parser.add_argument("--device-id", default=None, help="default: esp32-fake-<condition>")
    parser.add_argument("--num-windows", type=int, default=20)
    parser.add_argument("--window-interval-s", type=float, default=60.0,
                         help="simulated time between windows (default: 20 windows ~= 20 simulated minutes)")
    parser.add_argument("--sample-rate-hz", type=float, default=500.0, help="matches CONFIG_SAMPLE_RATE_HZ default")
    parser.add_argument("--window-size", type=int, default=256, help="matches CONFIG_SAMPLE_WINDOW_SIZE default")
    parser.add_argument("--motor-freq-hz", type=float, default=29.2, help="~1750 RPM small AC motor")
    parser.add_argument("--belt-freq-hz", type=float, default=8.0)
    parser.add_argument("--seed", type=int, default=None, help="for reproducible output")
    args = parser.parse_args()

    device_id = args.device_id or f"esp32-fake-{args.condition}"
    rng = np.random.default_rng(args.seed)
    conn = storage.connect(DB_PATH)

    session_start = time.time() - (args.num_windows - 1) * args.window_interval_s
    for i in range(args.num_windows):
        ax, ay, az = generate_window(
            rng, args.condition, args.sample_rate_hz, args.window_size,
            args.motor_freq_hz, args.belt_freq_hz,
        )
        payload = {"sample_rate_hz": args.sample_rate_hz, "ax": ax, "ay": ay, "az": az}
        received_at = session_start + i * args.window_interval_s
        window_id = storage.store_window(conn, device_id, payload, received_at=received_at)
        log.info("window_id=%d device=%s (%d/%d)", window_id, device_id, i + 1, args.num_windows)

    log.info("done -- wrote %d window(s) to %s. Run analyze_fft.py next.", args.num_windows, DB_PATH)


if __name__ == "__main__":
    main()
