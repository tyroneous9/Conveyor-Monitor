#!/usr/bin/env python3
"""Generate static report figures + a summary table from fft_results.

Reads backend/fft_backend.sqlite3 (real output of backend/analyze_fft.py --
no synthetic shortcuts taken here) and writes plain PNG plots plus a
markdown summary table to analysis/figures/. Nothing here is interactive:
matplotlib for the plots, scipy.stats for a real significance test on the
healthy/worn separation, output committed as static images for the README.

Usage:
    pip install -r requirements.txt
    python3 generate_figures.py [--healthy-device ID] [--worn-device ID]
"""

import argparse
import json
import os
import sqlite3

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from scipy import stats

DEFAULT_DB_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "backend", "fft_backend.sqlite3"
)
DB_PATH = os.environ.get("FFT_DB_PATH", DEFAULT_DB_PATH)
FIG_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "figures")

HEALTHY_COLOR = "#2a78d6"
WORN_COLOR = "#d03b3b"


def fetch_window(conn, device_id):
    """First window for device_id, raw + spectrum, joined."""
    row = conn.execute(
        "SELECT r.sample_rate_hz, r.ay, f.freq_hz, f.fft_ay, f.peak_freq_hz, f.peak_amp "
        "FROM raw_windows r JOIN fft_results f ON f.window_id = r.id "
        "WHERE r.device_id = ? ORDER BY r.id LIMIT 1",
        (device_id,),
    ).fetchone()
    if row is None:
        raise SystemExit(f"no windows found for device_id={device_id!r}")
    return {
        "sample_rate_hz": row[0],
        "ay": json.loads(row[1]),
        "freq_hz": json.loads(row[2]),
        "fft_ay": json.loads(row[3]),
        "peak_freq_hz": row[4],
        "peak_amp": row[5],
    }


def fetch_series(conn, device_id):
    rows = conn.execute(
        "SELECT peak_freq_hz, peak_amp FROM fft_results WHERE device_id = ? ORDER BY id",
        (device_id,),
    ).fetchall()
    if not rows:
        raise SystemExit(f"no fft_results found for device_id={device_id!r}")
    return np.array([r[0] for r in rows]), np.array([r[1] for r in rows])


def plot_spectrum(h, w, out_path):
    fig, ax = plt.subplots(figsize=(9, 4.5), dpi=150)
    cutoff = next(i for i, f in enumerate(h["freq_hz"]) if f > 100) + 1

    ax.plot(h["freq_hz"][:cutoff], h["fft_ay"][:cutoff], color=HEALTHY_COLOR, lw=1.8,
            label=f"Healthy (peak {h['peak_freq_hz']:.1f} Hz, {h['peak_amp']:.2f} g)")
    ax.fill_between(h["freq_hz"][:cutoff], h["fft_ay"][:cutoff], color=HEALTHY_COLOR, alpha=0.12)

    ax.plot(w["freq_hz"][:cutoff], w["fft_ay"][:cutoff], color=WORN_COLOR, lw=1.8,
            label=f"Worn (peak {w['peak_freq_hz']:.1f} Hz, {w['peak_amp']:.2f} g)")
    ax.fill_between(w["freq_hz"][:cutoff], w["fft_ay"][:cutoff], color=WORN_COLOR, alpha=0.12)

    ax.annotate(f"{w['peak_freq_hz']:.1f} Hz\nbelt-pass, dominant",
                xy=(w["peak_freq_hz"], w["peak_amp"]), xytext=(w["peak_freq_hz"] + 6, w["peak_amp"]),
                fontsize=9, color=WORN_COLOR, va="center")
    ax.annotate(f"{h['peak_freq_hz']:.1f} Hz\nmotor fundamental",
                xy=(h["peak_freq_hz"], h["peak_amp"]), xytext=(h["peak_freq_hz"] + 6, h["peak_amp"] + 2),
                fontsize=9, color=HEALTHY_COLOR, va="center")

    ax.set_xlabel("Frequency (Hz)")
    ax.set_ylabel("Amplitude (g)")
    ax.set_title("Frequency spectrum: healthy vs. worn belt")
    ax.set_xlim(0, 100)
    ax.legend(frameon=False, loc="upper right")
    ax.spines[["top", "right"]].set_visible(False)
    ax.grid(axis="y", color="#e1e0d9", lw=0.8)
    ax.set_axisbelow(True)
    fig.tight_layout()
    fig.savefig(out_path)
    plt.close(fig)


def plot_waveform(h, w, out_path):
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.2), dpi=150, sharey=True)
    t_h = np.arange(len(h["ay"])) / h["sample_rate_hz"] * 1000
    t_w = np.arange(len(w["ay"])) / w["sample_rate_hz"] * 1000

    for ax, t, sig, color, label in (
        (axes[0], t_h, h["ay"], HEALTHY_COLOR, "Healthy"),
        (axes[1], t_w, w["ay"], WORN_COLOR, "Worn"),
    ):
        ax.plot(t, sig, color=color, lw=1.1)
        ax.fill_between(t, sig, color=color, alpha=0.10)
        ax.set_title(label, color=color, fontsize=11, loc="left")
        ax.set_xlabel("ms")
        ax.spines[["top", "right"]].set_visible(False)
        ax.grid(axis="y", color="#e1e0d9", lw=0.8)
        ax.set_axisbelow(True)
    axes[0].set_ylabel("g (raw, one axis: ay)")
    fig.suptitle("Raw time-domain signal (before any transform)", y=1.02)
    fig.tight_layout()
    fig.savefig(out_path, bbox_inches="tight")
    plt.close(fig)


def plot_repeatability(h_freq, h_amp, w_freq, w_amp, out_path):
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.6), dpi=150)
    n = np.arange(1, len(h_freq) + 1)

    axes[0].scatter(n, h_freq, color=HEALTHY_COLOR, s=28, label="Healthy")
    axes[0].scatter(np.arange(1, len(w_freq) + 1), w_freq, color=WORN_COLOR, s=28, label="Worn")
    axes[0].set_title("Peak frequency")
    axes[0].set_xlabel("window #")
    axes[0].set_ylabel("Hz")
    axes[0].legend(frameon=False)

    axes[1].scatter(n, h_amp, color=HEALTHY_COLOR, s=28, label="Healthy")
    axes[1].scatter(np.arange(1, len(w_amp) + 1), w_amp, color=WORN_COLOR, s=28, label="Worn")
    axes[1].set_title("Peak amplitude")
    axes[1].set_xlabel("window #")
    axes[1].set_ylabel("g")
    axes[1].legend(frameon=False)

    for ax in axes:
        ax.spines[["top", "right"]].set_visible(False)
        ax.grid(axis="y", color="#e1e0d9", lw=0.8)
        ax.set_axisbelow(True)
    fig.suptitle("Repeatability across independent windows", y=1.02)
    fig.tight_layout()
    fig.savefig(out_path, bbox_inches="tight")
    plt.close(fig)


def describe_test(healthy, worn, decimals):
    """Mann-Whitney U, not a t-test: a t-test estimates the difference in
    means relative to within-group variance, which is undefined when that
    variance is zero -- exactly what happens here, since peak frequency is
    quantized to an FFT bin and this dataset's jitter isn't enough to move
    any window to a different bin (scipy.stats.ttest_ind degenerates to
    t=inf with a precision-loss warning on this data -- confirmed, not
    hypothetical). Mann-Whitney is rank-based, needs no variance assumption,
    and is valid for both the degenerate (frequency) and continuous
    (amplitude) case alike, so both rows use the same, correct test."""
    fmt = "{:." + str(decimals) + "f}"
    healthy_str = f"{fmt.format(healthy.mean())} ± {fmt.format(healthy.std())}"
    worn_str = f"{fmt.format(worn.mean())} ± {fmt.format(worn.std())}"
    u, p = stats.mannwhitneyu(healthy, worn, alternative="two-sided")
    return healthy_str, worn_str, f"U={u:.0f}, p={p:.2e}"


def write_summary_table(h_freq, h_amp, w_freq, w_amp, out_path):
    freq_row = describe_test(h_freq, w_freq, decimals=2)
    amp_row = describe_test(h_amp, w_amp, decimals=2)

    lines = [
        "| Metric | Healthy (n={}) | Worn (n={}) | Mann-Whitney U |".format(len(h_freq), len(w_freq)),
        "|---|---|---|---|",
        "| Peak frequency (Hz) | {} | {} | {} |".format(*freq_row),
        "| Peak amplitude (g) | {} | {} | {} |".format(*amp_row),
    ]
    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--healthy-device", default="esp32-fake-healthy")
    parser.add_argument("--worn-device", default="esp32-fake-worn")
    args = parser.parse_args()

    os.makedirs(FIG_DIR, exist_ok=True)
    conn = sqlite3.connect(DB_PATH)

    h_window = fetch_window(conn, args.healthy_device)
    w_window = fetch_window(conn, args.worn_device)
    h_freq, h_amp = fetch_series(conn, args.healthy_device)
    w_freq, w_amp = fetch_series(conn, args.worn_device)

    plot_spectrum(h_window, w_window, os.path.join(FIG_DIR, "spectrum_comparison.png"))
    plot_waveform(h_window, w_window, os.path.join(FIG_DIR, "waveform_comparison.png"))
    plot_repeatability(h_freq, h_amp, w_freq, w_amp, os.path.join(FIG_DIR, "repeatability.png"))
    table_lines = write_summary_table(h_freq, h_amp, w_freq, w_amp, os.path.join(FIG_DIR, "summary_table.md"))

    print(f"wrote figures to {FIG_DIR}/")
    print("\n".join(table_lines))


if __name__ == "__main__":
    main()
