#!/usr/bin/env python3
"""Threshold-based fault classifier: does a window's spectrum look like the
healthy baseline, or does it look worn?

Results are stored in the `baselines` and `classifications` database tables.

Usage:
    python3 labels.py \\
        --healthy-range 2026-08-20T09:00 2026-08-20T11:00 \\
        --worn-range 2026-08-22T09:00 2026-08-22T11:00
    python3 classify_faults.py
"""

import argparse
import logging
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "backend"))
import storage # type: ignore

DEFAULT_DB_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "backend", "fft_db.sqlite3"
)
DB_PATH = os.environ.get("FFT_DB_PATH", DEFAULT_DB_PATH)
FIG_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "figures")
FEATURE_NAME = "belt_band_amplitude"

HEALTHY_COLOR = "#2dd62a"
WORN_COLOR = "#ff4800"

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("classify_faults")


def band_amplitude(freq_hz, fft_amp, center_hz, width_hz):
    """Sum of linear FFT magnitude within [center-width/2, center+width/2]
    Bands are used instead of bins to avoid possible jittering."""
    lo, hi = center_hz - width_hz / 2, center_hz + width_hz / 2
    return sum(a for f, a in zip(freq_hz, fft_amp) if lo <= f <= hi)


def compute_band_amplitudes(windows, band_center_hz, band_width_hz):
    """band_amplitude for every window, keyed by window_id, computed once
    and reused for the baseline fit, classification, and plotting."""
    return {c["window_id"]: band_amplitude(c["freq_hz"], c["fft_ay"], band_center_hz, band_width_hz) for c in windows}


def split_healthy(healthy_windows, baseline_fraction):
    """Split healthy windows (in window order) into the fraction used to
    fit the baseline and the remainder held out for evaluation."""
    n_fit = max(1, int(len(healthy_windows) * baseline_fraction))
    return healthy_windows[:n_fit], healthy_windows[n_fit:]


def compute_threshold(amplitudes, fit_windows, n_std):
    values = np.array([amplitudes[c["window_id"]] for c in fit_windows])
    mean, std = float(values.mean()), float(values.std())
    threshold = mean + n_std * std
    log.info(
        "baseline: %s = %.3f ± %.3f (n=%d fit windows), threshold=%.3f",
        FEATURE_NAME, mean, std, len(fit_windows), threshold,
    )
    return mean, std, threshold


def classify(conn, amplitudes, windows, threshold, persist):
    """Apply the threshold to each window to classsify it as worn if amplitude threshold is exceeded."""
    results = []
    for c in windows:
        value = amplitudes[c["window_id"]]
        predicted = "worn" if value > threshold else "healthy"
        if persist:
            storage.store_classification(conn, c["window_id"], value, threshold, predicted)
        results.append({"window_id": c["window_id"], "value": value, "predicted": predicted})
    return results


def confusion_counts(rows):
    """(true positive, true negative, false positive, false negative)
    counts, "worn" treated as the positive class."""
    tp = sum(1 for r in rows if r["true"] == "worn" and r["predicted"] == "worn")
    tn = sum(1 for r in rows if r["true"] == "healthy" and r["predicted"] == "healthy")
    fp = sum(1 for r in rows if r["true"] == "healthy" and r["predicted"] == "worn")
    fn = sum(1 for r in rows if r["true"] == "worn" and r["predicted"] == "healthy")
    return tp, tn, fp, fn


def write_report(rows, threshold, baseline_mean, baseline_std, n_std, n_healthy, n_worn, out_path):
    """Build the confusion matrix + accuracy/precision/recall.
    Output to: markdown table to out_path, AND return the (accuracy, precision, recall) tuple."""
    tp, tn, fp, fn = confusion_counts(rows)
    n = len(rows)
    accuracy = (tp + tn) / n if n else float("nan")
    precision = tp / (tp + fp) if (tp + fp) else float("nan")
    recall = tp / (tp + fn) if (tp + fn) else float("nan")

    lines = [
        f"Ground truth: {n_healthy} healthy window(s), {n_worn} worn window(s) "
        "(labeled via analysis/labels.py from operator-recorded recording sessions).",
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
    """Plot a scatter chart of the belt-pass band amplitude for each window with classification."""
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
    parser.add_argument("--band-center-hz", type=float, default=8.0, help="belt-pass frequency to search around")
    parser.add_argument("--band-width-hz", type=float, default=6.0,
                         help="wide enough to cover the jittered peak landing in an adjacent FFT bin")
    parser.add_argument("--n-std", type=float, default=3.0, help="threshold = baseline mean + n_std * baseline std")
    parser.add_argument("--baseline-fraction", type=float, default=0.7,
                         help="fraction of the healthy windows (in window order) used to fit the baseline; rest are held out for evaluation")
    args = parser.parse_args()

    os.makedirs(FIG_DIR, exist_ok=True)
    conn = storage.connect(DB_PATH)

    all_windows = storage.fetch_fft_results(conn)
    healthy_windows = [c for c in all_windows if c["label"] == "healthy"]
    worn_windows = [c for c in all_windows if c["label"] == "worn"]
    if not healthy_windows:
        raise SystemExit("no windows are labeled healthy; run analysis/labels.py first")
    if not worn_windows:
        raise SystemExit("no windows are labeled worn; run analysis/labels.py first")

    # 1. band amplitudes, computed once per labeled window (unlabeled
    # windows in all_windows are never scored, so skip them)
    amplitudes = compute_band_amplitudes(healthy_windows + worn_windows, args.band_center_hz, args.band_width_hz)

    # 2. split healthy windows into fit / held-out
    fit_windows, holdout_windows = split_healthy(healthy_windows, args.baseline_fraction)

    # 3. baseline statistics -> threshold
    mean, std, threshold = compute_threshold(amplitudes, fit_windows, args.n_std)
    storage.store_baseline(conn, FEATURE_NAME, mean, std, len(fit_windows))

    # 4. classify (fit windows are scored for the plot only, not persisted
    # or reported -- see classify()'s docstring)
    rows = [{**r, "true": "healthy", "role": "baseline-fit"}
            for r in classify(conn, amplitudes, fit_windows, threshold, persist=False)]

    for windows, true, role in (
        (holdout_windows, "healthy", "held-out"),
        (worn_windows, "worn", "evaluated"),
    ):
        classified = classify(conn, amplitudes, windows, threshold, persist=True)
        rows += [{**c, "true": true, "role": role} for c in classified]
        log.info("classified %d window(s) (%s)", len(classified), role)

    # 5. reports and plots
    labeled_rows = [r for r in rows if r["role"] != "baseline-fit"]
    report_lines, (accuracy, precision, recall) = write_report(
        labeled_rows, threshold, mean, std, args.n_std, len(healthy_windows), len(worn_windows),
        os.path.join(FIG_DIR, "classification_report.md"),
    )
    plot_classification(rows, threshold, mean, std, os.path.join(FIG_DIR, "classification.png"))

    print(f"wrote figures to {FIG_DIR}/")
    print("\n".join(report_lines))


if __name__ == "__main__":
    main()
