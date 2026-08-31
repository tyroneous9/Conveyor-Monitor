# Conveyor Monitor

Predictive maintenance for an industrial conveyor belt: an ESP32 samples
vibration off an MPU6050 accelerometer, streams it over MQTT to a
Raspberry Pi, and analyzes this data using FFT to predict belt wear.

## Analysis results

Output from `backend/analyze_fft.py` on 700 captures (350 healthy and 350
worn), plotted with `analysis/generate_figures.py`. Healthy and worn
captures come from one device, split by capture-time range rather than by
a separate device_id per condition (see `analysis/labels.py`).

![Frequency spectrum: healthy vs. worn belt](analysis/figures/spectrum_comparison.png)

The worn belt shows a clear peak at its belt-pass frequency; the motor's
own rotation frequency (29.3 Hz) barely moves between conditions. Same
story in the raw signal, before any transform, though it's much less
obvious there than in frequency space:

![Raw time-domain signal, healthy vs. worn](analysis/figures/waveform_comparison.png)

And it holds across all 350 independent captures per condition, not just
one cherry-picked pair:

![Repeatability across independent captures](analysis/figures/repeatability.png)

| Metric | Healthy (n=350) | Worn (n=350) | Mann-Whitney U |
|---|---|---|---|
| Peak frequency (Hz) | 28.04 ± 4.35 | 9.01 ± 4.57 | U=116793, p=1.93×10⁻¹¹⁰ |
| Peak amplitude (g) | 3.88 ± 1.13 | 22.17 ± 7.93 | U=6149, p=2.88×10⁻⁹⁴ |

Mann-Whitney U instead of a t-test, because peak frequency is
bin-quantized — it clusters at a handful of discrete FFT bins rather than
varying continuously, which breaks a t-test's normality assumption.
Mann-Whitney is rank-based and doesn't need that assumption, so it's the
one test valid for both rows. Neither U sits at its extreme (122,500 or 0
of 122,500 possible healthy/worn pairs): a handful of captures on each side
land closer to the other condition than to their own — an early-stage-wear
capture that barely registers, a healthy capture with a transient noise
burst. The separation is still overwhelming (both p-values are
effectively zero), just not the textbook-perfect kind you'd be right to be
suspicious of.

Regenerate with `pip install -r analysis/requirements.txt && python3
analysis/generate_figures.py --device-id <id> --healthy-range <start> <end>
--worn-range <start> <end>` (timestamps are unix epoch or ISO 8601).

## Fault classification

A first pass at actually calling a capture healthy or worn, not just
computing its spectrum: `analysis/classify_faults.py`. It's deliberately
the simplest thing that could work, well short of anomaly detection or a
trained model — sum the FFT amplitude in a band around the belt-pass
frequency and compare it to a baseline (mean + 3 standard deviations) fit
on 70% of the healthy captures (244 of 350). The remaining 106 healthy
captures, plus all 350 worn captures, are held out for evaluation. The
baseline and every prediction get persisted to the `baselines` /
`classifications` tables in `backend/storage.py`, not just printed to the
console.

![Threshold classification: belt-pass band amplitude vs. baseline](analysis/figures/classification.png)

| | Predicted healthy | Predicted worn |
|---|---|---|
| **True healthy** | 101 | 5 |
| **True worn** | 27 | 323 |

93.0% accuracy on the 456 captures never used to set the threshold (98.5%
precision, 92.3% recall) — a handful of false alarms on healthy captures,
and it misses about 8% of worn ones, mostly the early-stage-wear cases
whose belt-pass amplitude hasn't climbed far enough above baseline yet to
clear a threshold set from healthy data alone. That's a reasonable failure
mode for a single-feature threshold, and the kind of thing you'd want to
know before trusting it: this is one belt on one capture session, so it
doesn't yet say the threshold holds under a different load, speed, or
belt, or that wear always shows up through this exact feature. Broader
validation across more sessions is what turns this from "a reasonable
first classifier" into one trusted in production (see [Status](#status)).

Regenerate with `python3 analysis/classify_faults.py --device-id <id>
--healthy-range <start> <end> --worn-range <start> <end>`.

## Architecture

```mermaid
flowchart LR
    MPU["MPU6050<br/>accelerometer"] -->|I2C| ESP["ESP32 firmware<br/>esp_timer @ 500Hz<br/>double-buffered captures"]
    ESP -->|"MQTT, QoS 1<br/>JSON capture"| Broker[["MQTT broker"]]
    Broker --> Ingest["ingest.py"]
    Ingest -->|raw_windows| DB[("SQLite")]
    DB -->|unanalyzed captures| Analyze["analyze_fft.py"]
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
main/            ESP-IDF firmware: fixed-rate sampling, capture buffering, MQTT publish
components/      MPU6050 I2C driver + vendored esp-mqtt / ethernet_init
backend/         ingest.py, analyze_fft.py, storage.py (SQLite schema)
analysis/        Notebook, static report figures, and the threshold classifier
deploy/          Mosquitto config + systemd units for running the broker and
                 backend as boot-persistent services on the Pi
```

## Getting started

**Firmware** — configure via `idf.py menuconfig` (WiFi credentials, MQTT
broker URI, MPU6050 I2C pins, `CONFIG_SAMPLE_RATE_HZ` /
`CONFIG_SAMPLE_WINDOW_SIZE`), then `idf.py build flash monitor`.

**Backend**, on the Pi or wherever the broker runs. For a one-off run:

```
pip install -r backend/requirements.txt
MQTT_BROKER_HOST=<broker-host> python3 backend/ingest.py       # long-running
python3 backend/analyze_fft.py --watch 30                      # or run once without --watch
```

For a Pi that should keep running this unattended (survives reboots and
process crashes), see `deploy/` — Mosquitto config plus systemd units that
install a local broker and both scripts as boot-persistent services.

**Analysis** — `pip install -r analysis/requirements.txt`; open
`analysis/explore_spectra.ipynb` for interactive exploration, or run
`python3 analysis/generate_figures.py` (see [Analysis
results](#analysis-results) for the required flags) to regenerate the
static report figures.

## Design decisions

A few choices here aren't the obvious first pick, so they're worth
explaining.

**Sampling is timer-driven, not delay-driven, and double-buffered.**
A naive `vTaskDelay` loop looks fine until you check the numbers: this
board's FreeRTOS tick runs at 100Hz (10ms resolution), which rounds a
500Hz/2ms sample period down to zero ticks and samples uncontrolled.
So the firmware uses `esp_timer` instead — hardware-backed, independent of
the FreeRTOS tick — to fire a minimal read-and-store callback at a fixed
rate, alternating between two capture buffers. JSON serialization and the
network publish run in a separate task, so a stalled MQTT publish never
delays the next sample.

**Ingestion and analysis run as separate processes.** `ingest.py` does one
thing: validate an incoming capture and write it to SQLite. `analyze_fft.py`
has no MQTT client at all — it's a standalone script that polls the
database for captures without a matching result, computes the spectrum, and
writes it back. A failure in one never blocks the other, and re-running
analysis is safe because it skips captures it's already processed, not
because you have to remember to be careful with it.

**SQLite over Postgres.** One writer at a time, one row per capture, no
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
The data behind it is from one conveyor and one capture session — not
enough to trust a black-box model not to overfit to that specific belt,
load, and speed, and a black-box model's weights don't announce when
they've done so. A threshold on one physically meaningful feature stays
inspectable, and recalibrating it as more sessions get captured is a
number change, not a retrain.

**The network is a phone hotspot, on purpose.** The primary WiFi here
enforces WPA3-only auth, and this board's (an original ESP32) WPA3 support
doesn't reliably negotiate it. Early testing also hit connection failures
against a public MQTT broker over that hotspot, which turned out to be the
carrier's traffic shaping dropping plain, non-standard-port TCP rather than
anything wrong with the broker itself. Running the broker locally on the
same hotspot network (see `deploy/`) keeps traffic off the carrier's WAN
entirely, which sidesteps both problems without touching router config I
don't control.

**A network drop queues instead of dropping the capture.** QoS 1 alone only
guarantees delivery of messages the client actually attempts to send; it
doesn't help if the firmware refuses to publish at all while disconnected,
which is what it used to do. Now `esp_mqtt_client_publish` is called
unconditionally, and the client's own outbox — capped at 8 captures' worth
of JSON, so a stuck connection can't grow it unbounded on a
memory-constrained device — holds anything sent while offline and flushes
it once auto-reconnect (2s retry, down from the 10s default) brings the
link back. A hotspot blip now costs a delay, not silently lost data; only
an outage longer than the outbox can hold still drops captures, and does so
loudly (logged, not silent).

## Status

| Component | State |
|---|---|
| Firmware: fixed-rate sampling into fixed-length captures, JSON publish | Implemented, code-reviewed |
| MQTT transport: topics, QoS, persistent session, bounded offline outbox, fast reconnect | Implemented |
| Ingestion (`ingest.py`) | Implemented |
| FFT analysis (`analyze_fft.py`) | Implemented, verified end-to-end, including idempotency on re-run |
| Local broker + backend deployment (`deploy/`: Mosquitto config, systemd units) | Implemented |
| Spectrum exploration notebook | Implemented, executes cleanly against the schema |
| Static report figures (`generate_figures.py`) | Implemented — see [Analysis results](#analysis-results) |
| Threshold fault classifier (`classify_faults.py`) | Implemented, 93.0% accuracy (98.5% precision, 92.3% recall) on held-out data — see [Fault classification](#fault-classification). One belt/session; broader validation across loads, speeds, and belts still open |
| Statistical anomaly detection / supervised classification | Not started — the natural next step after the threshold classifier, worth doing only if it proves too brittle across more sessions |

## Roadmap

Capture more sessions (different loads, speeds, times of day) to see
whether the threshold in [Fault classification](#fault-classification)
holds outside the one belt/session it's been fit and evaluated on so far.
Anomaly detection or a supervised model are only worth reaching for after
that, and only if the threshold proves too brittle in practice.
