# Conveyor Monitor

Predictive maintenance for an industrial conveyor belt. An ESP32 samples
vibration off an MPU6050 accelerometer, streams it over MQTT to a Raspberry
Pi, and a separate batch job turns each window into a frequency spectrum via
FFT. The idea is to catch belt wear from how the vibration signature drifts
over time, before it turns into downtime.

This repo covers the sensing → transport → storage → spectrum pipeline,
plus a first threshold-based classifier described below. That classifier
has only ever seen synthetic data — validating it against a real belt is
the next phase (see [Status](#status)).

## Analysis results

Real output from `backend/analyze_fft.py` on 200 synthetic windows (100
healthy, 100 worn — see [Status](#status) for why it's synthetic), plotted
with `analysis/generate_figures.py` (matplotlib + scipy, static PNGs,
nothing fancier).

Motor RPM, and more weakly belt-pass frequency, both drift a bit from run
to run on a real machine, so I built that jitter into the synthetic
generator (`backend/seed_samples.py`) instead of freezing peak frequency at
one value.

![Frequency spectrum: healthy vs. worn belt](analysis/figures/spectrum_comparison.png)

The belt-pass frequency (7.8 Hz) is where a worn belt actually shows up —
the motor's own rotation frequency (29.3 Hz) barely moves between
conditions. Same story in the raw signal, before any transform, though it's
much less obvious there than in frequency space:

![Raw time-domain signal, healthy vs. worn](analysis/figures/waveform_comparison.png)

And it holds across all 100 independent windows per condition, not just one
cherry-picked pair:

![Repeatability across independent windows](analysis/figures/repeatability.png)

| Metric | Healthy (n=100) | Worn (n=100) | Mann-Whitney U |
|---|---|---|---|
| Peak frequency (Hz) | 29.34 ± 1.17 | 8.12 ± 0.72 | U=10000, p=2.67×10⁻³⁸ |
| Peak amplitude (g) | 3.01 ± 0.25 | 21.03 ± 1.62 | U=0, p=2.56×10⁻³⁴ |

I used Mann-Whitney U instead of a t-test because peak frequency is
bin-quantized — it clusters at a handful of discrete FFT bins rather than
varying continuously, which breaks a t-test's normality assumption even
with genuine variance present. (An earlier version of the generator had
essentially zero variance in both groups, a more severe case of the same
issue: `scipy.stats.ttest_ind` degenerated to `t=inf` with a precision-loss
warning.) Mann-Whitney is rank-based and doesn't need that assumption, so
it's the one test valid for both rows here. U=10000 and U=0 both mean
complete separation: every one of the 100 healthy windows beat every one of
the 100 worn windows, on each metric.

Regenerate with `pip install -r analysis/requirements.txt && python3
analysis/generate_figures.py`.

## Fault classification

A first pass at actually calling a window healthy or worn, not just
computing its spectrum: `analysis/classify_faults.py`. It's deliberately
the simplest thing that could work, well short of anomaly detection or a
trained model — sum the FFT amplitude in a band around the belt-pass
frequency and compare it to a baseline (mean + 3 standard deviations) fit
on 70% of the healthy device's windows. The other 30% is held out for
evaluation. The baseline and every prediction get persisted to the
`baselines` / `classifications` tables in `backend/storage.py`, not just
printed to the console.

![Threshold classification: belt-pass band amplitude vs. baseline](analysis/figures/classification.png)

| | Predicted healthy | Predicted worn |
|---|---|---|
| **True healthy** | 30 | 0 |
| **True worn** | 0 | 100 |

100% accuracy on the 130 windows never used to set the threshold. That
number only says the threshold works given this exact synthetic model,
though — it doesn't say anything about a real belt, which might not
separate this cleanly, might drift with load or speed, or might show wear
through a completely different feature. Real hardware data is what turns
this from "a reasonable first classifier" into a validated one (see
[Status](#status)).

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

Ingestion and analysis are two separate processes that only ever talk
through the database — no MQTT, no shared queue between them. More on why
below.

## Repo layout

```
main/            ESP-IDF firmware: fixed-rate sampling, windowing, MQTT publish
components/      MPU6050 I2C driver + vendored esp-mqtt / ethernet_init
backend/         ingest.py, analyze_fft.py, storage.py (SQLite schema)
analysis/        Notebook, static report figures, and the threshold classifier
```

## Design decisions

A few choices here aren't the obvious first pick, so they're worth
explaining.

**Sampling is timer-driven, not delay-driven, and double-buffered.**
A naive `vTaskDelay` loop looks fine until you check the numbers: this
board's FreeRTOS tick runs at 100Hz (10ms resolution), which rounds a
500Hz/2ms sample period down to zero ticks and samples uncontrolled.
So the firmware uses `esp_timer` instead — hardware-backed, independent of
the FreeRTOS tick — to fire a minimal read-and-store callback at a fixed
rate, alternating between two window buffers. JSON serialization and the
network publish run in a separate task, so a stalled MQTT publish never
delays the next sample.

**Ingestion and analysis run as separate processes.** `ingest.py` does one
thing: validate an incoming window and write it to SQLite. `analyze_fft.py`
has no MQTT client at all — it's a standalone script that polls the
database for windows without a matching result, computes the spectrum, and
writes it back. A failure in one never blocks the other, and re-running
analysis is safe because it skips windows it's already processed, not
because you have to remember to be careful with it.

**SQLite over Postgres.** One writer at a time, one row per window, no
daemon competing with the MQTT broker for a Raspberry Pi's limited
resources. WAL mode is on so notebooks can read the file while a writer is
still appending. Two tables — `raw_windows` and `fft_results`, linked by
`window_id` — keep raw data and derived spectra separate, so neither ever
overwrites the other.

**QoS and session state are matched end to end.** MQTT's effective delivery
guarantee is `min(publish QoS, subscribe QoS)`, so publishing at QoS 1 buys
you nothing if the subscriber is listening at QoS 0 — an easy mismatch to
introduce, since nothing errors when it happens. The ingestion client
subscribes at QoS 1 with a stable `client_id` and `clean_session=False`, so
the broker holds onto messages published while it's offline instead of
dropping them.

**The classifier is a threshold, not a trained model.** That's deliberate.
With only synthetic "worn" data — a hand-picked signal model, not an
observed real belt — a fancier classifier would fit that assumption just
as confidently as a threshold does, just less visibly: a black-box model's
weights don't announce that they're wrong. A threshold on one physically
meaningful feature is inspectable, and recalibrating it once real data
exists is a number change, not a retrain.

**The network is a phone hotspot, on purpose.** The primary WiFi here
enforces WPA3-only auth, and this board's (an original ESP32) WPA3 support
doesn't reliably negotiate it. Early testing also hit connection failures
against a public MQTT broker over that hotspot, which turned out to be the
carrier's traffic shaping dropping plain, non-standard-port TCP rather than
anything wrong with the broker itself. Running the broker locally on the
same hotspot network keeps traffic off the carrier's WAN entirely, which
sidesteps both problems without touching router config I don't control.

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
| Statistical anomaly detection / supervised classification | Not started — the natural next step after the threshold classifier, worth doing only if it proves too brittle on real data |

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

Stand up the local broker → validate the firmware on real hardware → run a
capture session to pick a sample rate against the conveyor's actual
mechanical frequencies (500Hz is a placeholder right now) → capture a
known-healthy baseline from the real belt → re-fit and re-evaluate
`classify_faults.py`'s threshold against that real data instead of the
synthetic model it's only ever seen. Anomaly detection or a supervised
model are only worth reaching for after that, and only if the threshold
turns out to be too brittle in practice.
