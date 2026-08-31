# Conveyor Monitor

Predictive maintenance for an industrial conveyor belt: a ESP32 samples vibration
off an MPU6050 accelerometer, streams it over MQTT to a
Raspberry Pi, and a decoupled batch job turns each window into a frequency
spectrum via FFT. The eventual goal is catching belt wear from how the
vibration signature drifts over time, before it causes downtime — this repo
covers the sensing → transport → storage → spectrum pipeline, plus a first
threshold-based classifier (below) that's only ever seen synthetic data;
validating it against a real belt is the next phase (see [Status](#status)).

## Analysis results

Real output from `backend/analyze_fft.py` on 200 windows (100 healthy, 100
worn — synthetic test data, see [Status](#status)), plotted with
`analysis/generate_figures.py` (matplotlib + scipy, static PNGs, no
interactivity or external hosting involved). Frequency jitter between
windows is sized to occasionally cross an FFT bin boundary (motor RPM and,
more weakly, belt-pass frequency both drift run to run on a real machine —
see `backend/seed_sample_data.py`), so peak frequency isn't a frozen
constant here the way an earlier, less realistic pass had it.

![Frequency spectrum: healthy vs. worn belt](analysis/figures/spectrum_comparison.png)

The belt-pass frequency (7.8 Hz) is where a worn belt actually shows up —
the motor's own rotation frequency (29.3 Hz) barely moves between
conditions. Same finding in the raw signal, before any transform, though
far less obvious by eye than in frequency space:

![Raw time-domain signal, healthy vs. worn](analysis/figures/waveform_comparison.png)

Across all 100 independent windows per condition, not just one cherry-picked
window:

![Repeatability across independent windows](analysis/figures/repeatability.png)

| Metric | Healthy (n=100) | Worn (n=100) | Mann-Whitney U |
|---|---|---|---|
| Peak frequency (Hz) | 29.34 ± 1.17 | 8.12 ± 0.72 | U=10000, p=2.67×10⁻³⁸ |
| Peak amplitude (g) | 3.01 ± 0.25 | 21.03 ± 1.62 | U=0, p=2.56×10⁻³⁴ |

Mann-Whitney U, not a t-test: peak frequency is bin-quantized (clustered at
a handful of discrete FFT bins, not continuous), which violates a t-test's
approximate-normality assumption even with real variance present — and an
earlier pass had exactly zero variance in both groups from under-sized
jitter, which is flatly undefined for a t-test (`scipy.stats.ttest_ind`
degenerated to `t=inf` with a precision-loss warning; see `TODO.md`).
Mann-Whitney is rank-based and needs neither assumption, so the same test
applies validly to both rows. U=10000 and U=0 both mean *complete*
separation — every one of the 100 healthy windows' values beat every one of
the 100 worn windows', in each metric's respective direction.

Regenerate with `pip install -r analysis/requirements.txt && python3
analysis/generate_figures.py`.

## Fault classification

A first pass at actually calling a window healthy or worn, not just
computing its spectrum — `analysis/classify_faults.py`. Deliberately the
simplest option in README §4's own staged progression (threshold on one
feature, before reaching for anomaly detection or a trained model): sum the
FFT amplitude in a band around the belt-pass frequency, compare it to a
baseline (mean + 3 standard deviations) fit on 70% of the healthy device's
windows, held out the other 30% for evaluation. Baseline and every
prediction are persisted (`baselines` / `classifications` tables in
`backend/storage.py`), not just printed.

![Threshold classification: belt-pass band amplitude vs. baseline](analysis/figures/classification.png)

| | Predicted healthy | Predicted worn |
|---|---|---|
| **True healthy** | 30 | 0 |
| **True worn** | 0 | 100 |

Accuracy 100%, on the 130 windows never used to set the threshold (30
held-out healthy + 100 worn) — but read that number for what it is, not
more: **evaluated only against the same hand-picked synthetic model used
everywhere else on this page.** It shows the threshold approach is sound
*given* that model, not that it will work on a real belt, which may not
separate this cleanly, may drift with load or speed, or may show wear as a
different feature entirely. Real hardware data is what turns this from "a
reasonable first classifier" into a validated one — see
[Status](#status) and `TODO.md`.

Regenerate with `python3 analysis/classify_faults.py`.

## Architecture

```mermaid
flowchart LR
    MPU["MPU6050<br/>accelerometer"] -->|I2C| ESP["ESP32 firmware<br/>esp_timer @ 500Hz<br/>double-buffered windows"]
    ESP -->|"MQTT, QoS 1<br/>JSON window"| Broker[["MQTT broker"]]
    Broker --> Ingest["ingest.py"]
    Ingest -->|raw_windows| DB[("SQLite")]
    DB -->|unanalyzed windows| Analyze["analyze_fft.py"]
    Analyze -->|fft_results| DB
    DB --> NB["analysis/*.ipynb"]
    DB -->|fft_results| Classify["classify_faults.py"]
    Classify -->|baselines,<br/>classifications| DB
```

Ingestion and analysis are deliberately two separate processes talking only
through the database, not through MQTT or a shared queue — see
[Design decisions](#design-decisions).

## Repo layout

```
main/            ESP-IDF firmware: fixed-rate sampling, windowing, MQTT publish
components/      MPU6050 I2C driver + vendored esp-mqtt / ethernet_init
backend/         ingest.py, analyze_fft.py, storage.py (SQLite schema)
analysis/        Notebook, static report figures, and the threshold classifier
TODO.md          Working engineering log: open items, decisions, rationale
```

## Design decisions

A few choices here aren't the first thing you'd reach for, so it's worth
saying why.

**Sampling is timer-driven, not delay-driven, and double-buffered.**
A naive `vTaskDelay`-based sample loop looks fine until you check the
numbers: this board's FreeRTOS tick is 100Hz (10ms resolution), which would
round a 500Hz/2ms sample period down to zero ticks and sample uncontrolled.
Firmware instead uses `esp_timer` — backed by hardware, independent of the
FreeRTOS tick — to fire a minimal read-and-store callback at a fixed rate,
alternating between two window buffers. The slower part (JSON serialization,
network publish) runs in a separate task, so a stalled MQTT publish never
delays the next sample.

**Ingestion and analysis are separate processes, not one pipeline.**
`ingest.py` does exactly one thing: validate an incoming window and write it
to SQLite. `analyze_fft.py` is a standalone script with no MQTT client at
all — it polls the database for windows without a matching result, computes
the spectrum, and writes it back. Failure in one never blocks the other, and
re-running analysis is safe by construction (it skips windows already
analyzed) rather than by convention.

**SQLite, not Postgres.** Single writer at a time, one row per window, no
daemon competing with the MQTT broker for a Raspberry Pi's resources.
WAL mode is enabled so notebooks can read the file while a writer is still
appending. Two tables — `raw_windows` and `fft_results`, linked by
`window_id` — so raw data and derived spectra are kept separately and
neither is ever overwritten by the other.

**QoS and session state are matched end-to-end, not just set once.**
MQTT's effective delivery guarantee is `min(publish QoS, subscribe QoS)`, so
publishing at QoS 1 does nothing if the subscriber listens at QoS 0 — an
easy mismatch to introduce without noticing, since nothing errors. The
ingestion client subscribes at QoS 1 with a stable `client_id` and
`clean_session=False`, so the broker holds messages published while it's
offline instead of silently dropping them.

**The classifier is a threshold, not a trained model — deliberately.** With
only synthetic "worn" data (a hand-picked signal model, not an observed real
belt), a fancier classifier — isolation forest, a supervised model — would
fit that assumption just as confidently as a threshold does, just less
visibly: a black-box model's weights don't tell you they're wrong until they
are. A threshold on one physically-meaningful feature (belt-pass band
amplitude vs. a stored baseline) is inspectable, and recalibrating it once
real data exists is a number change, not a retrain.

**The network is a phone hotspot, on purpose.** The primary WiFi network
here enforces WPA3-only auth, which this board's (original ESP32) software WPA3
implementation doesn't reliably negotiate. Early testing also hit
connection failures against a public MQTT broker over that hotspot — turned
out to be the carrier's traffic shaping dropping plain, non-standard-port
TCP, not the broker. Running the broker locally on the same hotspot network
keeps traffic from ever crossing onto the carrier's WAN, sidestepping both
problems without touching the router config.

## Status

| Component | State |
|---|---|
| Firmware: fixed-rate windowed sampling, JSON publish | Implemented, code-reviewed; not yet run on physical hardware (no toolchain in the dev environment used to write it) |
| MQTT transport: topics, QoS, persistent session | Implemented |
| Ingestion (`ingest.py`) | Implemented, tested against synthetic MQTT payloads |
| FFT analysis (`analyze_fft.py`) | Implemented, verified end-to-end against synthetic multi-window data, including idempotency on re-run |
| Local broker deployment | Not yet done — currently pointed at a public cloud broker as an interim step |
| Spectrum exploration notebook | Implemented, executes cleanly against the real schema |
| Static report figures (`generate_figures.py`) | Implemented, run against the real database — see [Analysis results](#analysis-results) |
| Threshold fault classifier (`classify_faults.py`) | Implemented, 100% accuracy on held-out synthetic data — see [Fault classification](#fault-classification). **Not validated against a real belt** |
| Statistical anomaly detection / supervised classification | Not started — README §4's steps 2–3, only worth reaching for if the threshold approach proves too brittle on real data |

## Getting started

**Firmware** — configure via `idf.py menuconfig` (WiFi credentials, MQTT
broker URI, MPU6050 I2C pins, `CONFIG_SAMPLE_RATE_HZ` /
`CONFIG_SAMPLE_WINDOW_SIZE`), then `idf.py build flash monitor`.

**Backend**, on the Pi or wherever the broker runs:

```
pip install -r backend/requirements.txt
MQTT_BROKER_HOST=<broker-host> python3 backend/ingest.py       # long-running
python3 backend/analyze_fft.py --watch 30                      # or run once without --watch
```

**Analysis** — `pip install -r analysis/requirements.txt`; open
`analysis/explore_spectra.ipynb` for interactive exploration, or run
`python3 analysis/generate_figures.py` to regenerate the static report
figures under [Analysis results](#analysis-results).

## Roadmap

Tracked in detail in [`TODO.md`](TODO.md); the near-term path is: stand up
the local broker → validate the firmware on real hardware → run a capture
session to pick a sample rate against the conveyor's actual mechanical
frequencies (currently a 500Hz placeholder) → capture a known-healthy
baseline spectrum from the real belt → re-fit and re-evaluate
`classify_faults.py`'s threshold against that real data, not the synthetic
model it's only ever seen → only then consider README §4's steps 2–3
(statistical anomaly detection, supervised classification) if the threshold
turns out to be too brittle in practice.
