#!/usr/bin/env python3
"""Threshold-based fault classifier: does a window's spectrum look like the
healthy baseline, or does it look worn?

Deliberately the *simplest* thing in README §4's own staged progression --
"1. Threshold/anomaly detection on one or two features... 2. Statistical
anomaly detection... if thresholds prove too brittle... 3. Supervised
classification... only once you have labeled examples". We have labels
here, but they're synthetic (backend/seed_sample_data.py's hand-picked
signal model, not an observed real belt) -- training a fancier model against
that would just be confidently wrong in a different, harder-to-notice way
than a threshold rule is. This stays interpretable: one feature (summed FFT
amplitude in a band around the belt-pass frequency), one threshold
(baseline mean + N standard deviations), inspectable and recalibratable the
moment real hardware data exists. Steps 2/3 remain available later -- see
TODO.md.

Baseline/classification results are stored in the `baselines` and
`classifications` tables (backend/storage.py) as durable, inspectable
artifacts, not numbers recomputed silently inside this script every run.

Methodology note: the baseline is fit on a held-out fraction of the
baseline device's own windows (--baseline-fraction, default 0.7, taken in
capture order) and evaluated against the *remaining* fraction plus every
other device's windows -- evaluating "does healthy data fall under
threshold" on the same windows used to set that threshold would be
circular for the healthy class.

Usage:
    pip install -r requirements.txt
    python3 classify_faults.py
    python3 classify_faults.py --baseline-device esp32-fake-healthy \\
        --evaluate-devices esp32-fake-healthy esp32-fake-worn
"""

import argparse
import logging
import os
import sys
import time

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "backend"))
import storage  # noqa: E402  (reuses the schema + write functions rather than duplicating them)

DEFAULT_DB_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "backend", "fft_backend.sqlite3"
)
DB_PATH = os.environ.get("FFT_DB_PATH", DEFAULT_DB_PATH)
FIG_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "figures")
FEATURE_NAME = "belt_band_amplitude"

HEALTHY_COLOR = "#2a78d6"
WORN_COLOR = "#d03b3b"

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("classify_faults")


def band_amplitude(freq_hz, fft_amp, center_hz, width_hz):
    """Sum of linear FFT magnitude within [center-width/2, center+width/2]
    -- a band, not a single bin, because realistic frequency jitter
    (backend/seed_sample_data.py) can shift the true peak to an adjacent
    bin between windows; a single-bin lookup would miss it."""
    lo, hi = center_hz - width_hz / 2, center_hz + width_hz / 2
    return sum(a for f, a in zip(freq_hz, fft_amp) if lo <= f <= hi)


def true_label(device_id):
    if "healthy" in device_id:
        return "healthy"
    if "worn" in device_id:
        return "worn"
    return None  # unknown ground truth -- real hardware, not synthetic


def compute_baseline(conn, device_id, band_center_hz, band_width_hz, baseline_fraction):
    windows = storage.fetch_fft_results(conn, device_id)
    if not windows:
        raise SystemExit(f"no fft_results found for device_id={device_id!r}")

    n_fit = max(1, int(len(windows) * baseline_fraction))
    fit_windows, holdout_windows = windows[:n_fit], windows[n_fit:]

    values = np.array([band_amplitude(w["freq_hz"], w["fft_ay"], band_center_hz, band_width_hz) for w in fit_windows])
    mean, std = float(values.mean()), float(values.std())
    storage.store_baseline(conn, device_id, FEATURE_NAME, mean, std, len(fit_windows))
    log.info(
        "baseline from %s: %s = %.3f ± %.3f (n=%d fit windows, %d held out)",
        device_id, FEATURE_NAME, mean, std, len(fit_windows), len(holdout_windows),
    )
    return mean, std, fit_windows, holdout_windows


def classify_windows(conn, windows, device_id, threshold, band_center_hz, band_width_hz):
    results = []
    for w in windows:
        value = band_amplitude(w["freq_hz"], w["fft_ay"], band_center_hz, band_width_hz)
        predicted = "worn" if value > threshold else "healthy"
        storage.store_classification(conn, w["window_id"], device_id, value, threshold, predicted)
        results.append({"window_id": w["window_id"], "value": value, "predicted": predicted})
    return results


def confusion_counts(rows):
    tp = sum(1 for r in rows if r["true"] == "worn" and r["predicted"] == "worn")
    tn = sum(1 for r in rows if r["true"] == "healthy" and r["predicted"] == "healthy")
    fp = sum(1 for r in rows if r["true"] == "healthy" and r["predicted"] == "worn")
    fn = sum(1 for r in rows if r["true"] == "worn" and r["predicted"] == "healthy")
    return tp, tn, fp, fn


def write_report(rows, threshold, baseline_mean, baseline_std, n_std, out_path):
    tp, tn, fp, fn = confusion_counts(rows)
    n = len(rows)
    accuracy = (tp + tn) / n if n else float("nan")
    precision = tp / (tp + fp) if (tp + fp) else float("nan")
    recall = tp / (tp + fn) if (tp + fn) else float("nan")

    lines = [
        "**Against synthetic labels only** (backend/seed_sample_data.py's hand-picked "
        "signal model) -- not validated against a real observed worn belt.",
        "",
        f"Threshold: `{FEATURE_NAME}` > {threshold:.3f} "
        f"(baseline {baseline_mean:.3f} + {n_std:g}×{baseline_std:.3f} std)",
        "",
        "| | Predicted healthy | Predicted worn |",
        "|---|---|---|",
        f"| **True healthy** | {tn} | {fp} |",
        f"| **True worn** | {fn} | {tp} |",
        "",
        f"Accuracy: {accuracy:.1%} · Precision: {precision:.1%} · Recall: {recall:.1%} (n={n})",
    ]
    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return lines, (accuracy, precision, recall)


def plot_classification(rows, threshold, baseline_mean, baseline_std, out_path):
    fig, ax = plt.subplots(figsize=(9, 4.5), dpi=150)

    markers = {"baseline-fit": "o", "held-out": "^", "evaluated": "s"}
    for role in ("baseline-fit", "held-out", "evaluated"):
        subset = [r for r in rows if r["role"] == role]
        if not subset:
            continue
        for label, color in (("healthy", HEALTHY_COLOR), ("worn", WORN_COLOR)):
            pts = [r for r in subset if r["true"] == label]
            if not pts:
                continue
            ax.scatter(
                [r["window_id"] for r in pts], [r["value"] for r in pts],
                color=color, marker=markers[role], s=32,
                facecolors=color if role != "held-out" else "none",
                edgecolors=color,
                label=f"{label} ({role})",
            )

    ax.axhline(threshold, color="black", lw=1.2, linestyle="-")
    ax.annotate(f"threshold = baseline + Nσ = {threshold:.2f}", xy=(0.01, threshold), xycoords=("axes fraction", "data"),
                xytext=(4, 4), textcoords="offset points", fontsize=9)
    ax.axhspan(baseline_mean - baseline_std, baseline_mean + baseline_std, color=HEALTHY_COLOR, alpha=0.08)

    ax.set_xlabel("window id")
    ax.set_ylabel(f"{FEATURE_NAME} (g)")
    ax.set_title("Threshold classification: belt-pass band amplitude vs. baseline")
    ax.legend(frameon=False, loc="upper left", fontsize=8, ncol=2)
    ax.spines[["top", "right"]].set_visible(False)
    ax.grid(axis="y", color="#e1e0d9", lw=0.8)
    ax.set_axisbelow(True)
    fig.tight_layout()
    fig.savefig(out_path)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--baseline-device", default="esp32-fake-healthy")
    parser.add_argument("--evaluate-devices", nargs="*", default=None,
                         help="default: every distinct device_id in fft_results")
    parser.add_argument("--band-center-hz", type=float, default=8.0,
                         help="assumed belt-pass frequency -- placeholder until real hardware data picks one (see TODO.md)")
    parser.add_argument("--band-width-hz", type=float, default=6.0,
                         help="wide enough to cover the jittered peak landing in an adjacent FFT bin")
    parser.add_argument("--n-std", type=float, default=3.0, help="threshold = baseline mean + n_std * baseline std")
    parser.add_argument("--baseline-fraction", type=float, default=0.7,
                         help="fraction of the baseline device's windows (in capture order) used to fit the baseline; rest are held out for evaluation")
    args = parser.parse_args()

    os.makedirs(FIG_DIR, exist_ok=True)
    conn = storage.connect(DB_PATH)

    mean, std, fit_windows, holdout_windows = compute_baseline(
        conn, args.baseline_device, args.band_center_hz, args.band_width_hz, args.baseline_fraction
    )
    threshold = mean + args.n_std * std

    if args.evaluate_devices is None:
        evaluate_devices = [r[0] for r in conn.execute("SELECT DISTINCT device_id FROM fft_results")]
    else:
        evaluate_devices = args.evaluate_devices

    rows = []
    for w in fit_windows:
        value = band_amplitude(w["freq_hz"], w["fft_ay"], args.band_center_hz, args.band_width_hz)
        rows.append({"window_id": w["window_id"], "value": value, "true": true_label(args.baseline_device),
                      "predicted": "worn" if value > threshold else "healthy", "role": "baseline-fit"})

    for device_id in evaluate_devices:
        windows = holdout_windows if device_id == args.baseline_device else storage.fetch_fft_results(conn, device_id)
        role = "held-out" if device_id == args.baseline_device else "evaluated"
        classified = classify_windows(conn, windows, device_id, threshold, args.band_center_hz, args.band_width_hz)
        for c in classified:
            rows.append({**c, "true": true_label(device_id), "role": role})
        log.info("classified %d window(s) from %s (%s)", len(classified), device_id, role)

    # Excludes role=="baseline-fit": those windows set the threshold, so
    # evaluating them against that same threshold is circular, not a real
    # test (fit windows still appear in the plot, for context, via `rows`).
    labeled_rows = [r for r in rows if r["true"] is not None and r["role"] != "baseline-fit"]
    report_lines, (accuracy, precision, recall) = write_report(
        labeled_rows, threshold, mean, std, args.n_std, os.path.join(FIG_DIR, "classification_report.md")
    )
    plot_classification(rows, threshold, mean, std, os.path.join(FIG_DIR, "classification.png"))

    print(f"wrote figures to {FIG_DIR}/")
    print("\n".join(report_lines))


if __name__ == "__main__":
    main()
