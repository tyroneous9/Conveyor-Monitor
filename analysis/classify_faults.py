#!/usr/bin/env python3
"""Threshold-based fault classifier: does a capture's spectrum look like the
healthy baseline, or does it look worn?

Deliberately the *simplest* thing in README §4's own staged progression --
"1. Threshold/anomaly detection on one or two features... 2. Statistical
anomaly detection... if thresholds prove too brittle... 3. Supervised
classification... only once you have labeled examples". A fancier model
would fit the quirks of the belt/load/speed combination it's trained on
just as confidently as a threshold does, just less visibly -- a black-box
model's weights don't announce that they're overfit. This stays
interpretable: one feature (summed FFT amplitude in a band around the
belt-pass frequency), one threshold (baseline mean + N standard
deviations), recalibratable as more sessions get captured -- a number
change, not a retrain. Steps 2/3 remain available later if this proves too
brittle.

Ground truth comes from operator-recorded capture sessions, not device_id:
one physical device (--device-id) gets moved between a known-healthy and a
known-worn belt on different runs, and a capture is labeled by which
--healthy-range / --worn-range its capture timestamp falls in.

Baseline/classification results are stored in the `baselines` and
`classifications` tables (backend/storage.py) as durable, inspectable
artifacts, not numbers recomputed silently inside this script every run.

Methodology note: the baseline is fit on a held-out fraction of the
healthy range's captures (--baseline-fraction, default 0.7, taken in
capture order) and evaluated against the *remaining* healthy fraction plus
every worn-range capture -- evaluating "does healthy data fall under
threshold" on the same captures used to set that threshold would be
circular for the healthy class.

Usage:
    pip install -r requirements.txt
    python3 classify_faults.py --device-id esp32-a1b2c3 \\
        --healthy-range 2026-08-20T09:00 2026-08-20T11:00 \\
        --worn-range 2026-08-22T09:00 2026-08-22T11:00

For local dev/testing without real hardware, backend/seed_samples.py can
seed a similar two-session dataset under one device_id.
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
import storage  # noqa: E402  (reuses the schema + write functions rather than duplicating them)
import labels  # noqa: E402

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
    -- a band, not a single bin, because motor/belt speed drifts run to
    run (see README) and can shift the true peak to an adjacent bin
    between captures; a single-bin lookup would miss it."""
    lo, hi = center_hz - width_hz / 2, center_hz + width_hz / 2
    return sum(a for f, a in zip(freq_hz, fft_amp) if lo <= f <= hi)


def compute_baseline(healthy_captures, band_center_hz, band_width_hz, baseline_fraction):
    n_fit = max(1, int(len(healthy_captures) * baseline_fraction))
    fit_captures, holdout_captures = healthy_captures[:n_fit], healthy_captures[n_fit:]

    values = np.array([band_amplitude(c["freq_hz"], c["fft_ay"], band_center_hz, band_width_hz) for c in fit_captures])
    mean, std = float(values.mean()), float(values.std())
    log.info(
        "baseline: %s = %.3f ± %.3f (n=%d fit captures, %d held out)",
        FEATURE_NAME, mean, std, len(fit_captures), len(holdout_captures),
    )
    return mean, std, fit_captures, holdout_captures


def classify_captures(conn, captures, device_id, threshold, band_center_hz, band_width_hz):
    results = []
    for c in captures:
        value = band_amplitude(c["freq_hz"], c["fft_ay"], band_center_hz, band_width_hz)
        predicted = "worn" if value > threshold else "healthy"
        storage.store_classification(conn, c["capture_id"], device_id, value, threshold, predicted)
        results.append({"capture_id": c["capture_id"], "value": value, "predicted": predicted})
    return results


def confusion_counts(rows):
    tp = sum(1 for r in rows if r["true"] == "worn" and r["predicted"] == "worn")
    tn = sum(1 for r in rows if r["true"] == "healthy" and r["predicted"] == "healthy")
    fp = sum(1 for r in rows if r["true"] == "healthy" and r["predicted"] == "worn")
    fn = sum(1 for r in rows if r["true"] == "worn" and r["predicted"] == "healthy")
    return tp, tn, fp, fn


def fmt_ranges(ranges):
    return ", ".join(f"[{a:.0f}, {b:.0f}]" for a, b in ranges)


def write_report(rows, threshold, baseline_mean, baseline_std, n_std, device_id, healthy_ranges, worn_ranges, out_path):
    tp, tn, fp, fn = confusion_counts(rows)
    n = len(rows)
    accuracy = (tp + tn) / n if n else float("nan")
    precision = tp / (tp + fp) if (tp + fp) else float("nan")
    recall = tp / (tp + fn) if (tp + fn) else float("nan")

    lines = [
        f"Ground truth: device `{device_id}`, healthy range(s) {fmt_ranges(healthy_ranges)}, "
        f"worn range(s) {fmt_ranges(worn_ranges)} (operator-recorded capture sessions).",
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
                [r["capture_id"] for r in pts], [r["value"] for r in pts],
                color=color, marker=markers[role], s=32,
                facecolors=color if role != "held-out" else "none",
                edgecolors=color,
                label=f"{label} ({role})",
            )

    ax.axhline(threshold, color="black", lw=1.2, linestyle="-")
    ax.annotate(f"threshold = baseline + Nσ = {threshold:.2f}", xy=(0.01, threshold), xycoords=("axes fraction", "data"),
                xytext=(4, 4), textcoords="offset points", fontsize=9)
    ax.axhspan(baseline_mean - baseline_std, baseline_mean + baseline_std, color=HEALTHY_COLOR, alpha=0.08)

    ax.set_xlabel("capture id")
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
    labels.add_session_args(parser)
    parser.add_argument("--band-center-hz", type=float, default=8.0, help="belt-pass frequency to search around")
    parser.add_argument("--band-width-hz", type=float, default=6.0,
                         help="wide enough to cover the jittered peak landing in an adjacent FFT bin")
    parser.add_argument("--n-std", type=float, default=3.0, help="threshold = baseline mean + n_std * baseline std")
    parser.add_argument("--baseline-fraction", type=float, default=0.7,
                         help="fraction of the healthy range's captures (in capture order) used to fit the baseline; rest are held out for evaluation")
    args = parser.parse_args()

    os.makedirs(FIG_DIR, exist_ok=True)
    conn = storage.connect(DB_PATH)

    device_id = labels.resolve_device_id(conn, "fft_results", args.device_id)
    healthy_ranges = labels.parse_ranges(args.healthy_range)
    worn_ranges = labels.parse_ranges(args.worn_range)
    if not healthy_ranges:
        raise SystemExit("--healthy-range is required (repeatable), e.g. --healthy-range 2026-08-20T09:00 2026-08-20T11:00")
    if not worn_ranges:
        raise SystemExit("--worn-range is required (repeatable), e.g. --worn-range 2026-08-22T09:00 2026-08-22T11:00")

    all_captures = storage.fetch_fft_results(conn, device_id)
    for c in all_captures:
        c["label"] = labels.label_for(c["received_at"], healthy_ranges, worn_ranges)
    healthy_captures = [c for c in all_captures if c["label"] == "healthy"]
    worn_captures = [c for c in all_captures if c["label"] == "worn"]
    if not healthy_captures:
        raise SystemExit(f"no captures from device_id={device_id!r} fall inside --healthy-range")
    if not worn_captures:
        raise SystemExit(f"no captures from device_id={device_id!r} fall inside --worn-range")

    mean, std, fit_captures, holdout_captures = compute_baseline(
        healthy_captures, args.band_center_hz, args.band_width_hz, args.baseline_fraction
    )
    threshold = mean + args.n_std * std
    storage.store_baseline(conn, device_id, FEATURE_NAME, mean, std, len(fit_captures))

    rows = []
    for c in fit_captures:
        value = band_amplitude(c["freq_hz"], c["fft_ay"], args.band_center_hz, args.band_width_hz)
        rows.append({"capture_id": c["capture_id"], "value": value, "true": "healthy",
                      "predicted": "worn" if value > threshold else "healthy", "role": "baseline-fit"})

    for captures, true, role in (
        (holdout_captures, "healthy", "held-out"),
        (worn_captures, "worn", "evaluated"),
    ):
        classified = classify_captures(conn, captures, device_id, threshold, args.band_center_hz, args.band_width_hz)
        for c in classified:
            rows.append({**c, "true": true, "role": role})
        log.info("classified %d capture(s) (%s)", len(classified), role)

    # Excludes role=="baseline-fit": those captures set the threshold, so
    # evaluating them against that same threshold is circular, not a real
    # test (fit captures still appear in the plot, for context, via `rows`).
    labeled_rows = [r for r in rows if r["role"] != "baseline-fit"]
    report_lines, (accuracy, precision, recall) = write_report(
        labeled_rows, threshold, mean, std, args.n_std, device_id, healthy_ranges, worn_ranges,
        os.path.join(FIG_DIR, "classification_report.md"),
    )
    plot_classification(rows, threshold, mean, std, os.path.join(FIG_DIR, "classification.png"))

    print(f"wrote figures to {FIG_DIR}/")
    print("\n".join(report_lines))


if __name__ == "__main__":
    main()
