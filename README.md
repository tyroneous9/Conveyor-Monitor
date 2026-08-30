# Vibration-Based Predictive Maintenance (MPU6050 + ESP32 + Raspberry Pi)

## 1. What This Project Actually Is

Strip away the sensors and protocols and this project is really an exercise in
**predictive maintenance**: catching a mechanical fault (belt wear) *before*
it causes downtime, by watching how a machine vibrates over time and noticing
when the vibration signature drifts away from "normal."

That framing matters more than any single library choice. Every design
decision below should be judged against one question: *does this help us
tell "healthy belt" apart from "worn belt" reliably, over time, without
drowning in noise or false alarms?*

The system has four concerns, and it's worth treating them as four separate
learning topics rather than one big blob:

1. **Sensing** — turning physical vibration into digital numbers (MPU6050 on ESP32)
2. **Transport** — getting those numbers off the device reliably (MQTT)
3. **Signal processing** — turning raw motion samples into a frequency picture (FFT)
4. **Inference** — deciding, from that frequency picture, whether the belt looks worn (the "predictive model")

Most beginners jump straight to "how do I do FFT in Python" and skip the
parts that actually determine whether the FFT output means anything. Sections
2 and 3 below are deliberately upstream of the FFT for that reason.

---

## 2. Sensing: Why Sampling Strategy Comes Before Everything Else

The MPU6050 is a 6-axis IMU (3-axis accelerometer + 3-axis gyroscope). For
vibration analysis, you care almost entirely about the **accelerometer**,
because vibration is oscillating acceleration.

### The concept to internalize: the Nyquist limit

You cannot analyze frequencies in your signal that are higher than half your
sampling rate. This is the single most common mistake in beginner vibration
projects: people sample at some convenient rate (say 50 Hz), then wonder why
their FFT doesn't show the 40 Hz bearing/belt frequency clearly, or why it
aliases into a completely different, misleading frequency.

**Standard practice:** identify the mechanical frequencies you care about
first, *then* pick a sample rate at least 2–4x higher (2x is the bare
mathematical minimum; 4x+ gives you margin and cleaner FFT bins).

- Motor RPM → shaft rotation frequency (RPM / 60)
- Belt length and pulley diameters → belt-pass frequency
- These are usually in the 5–200 Hz range for small motors/fans

If you don't know these numbers yet, that's fine — it's part of the project.
Run a baseline capture, look at the raw time-domain signal, and let it guide
your sample rate rather than guessing.

### Fixed-rate sampling matters more than raw speed

For FFT to be meaningful, samples need to be evenly spaced in time. An
`I2C read + delay(20)` loop on the ESP32 will *drift* — I2C transaction time
isn't constant, so your "50 Hz" loop isn't really 50 Hz. Standard fixes:

- Use a hardware timer interrupt to trigger reads at a fixed interval
- Or timestamp every sample and resample/interpolate to a uniform grid before FFT
- Log the actual achieved sample rate, don't just assume the nominal one

This is worth learning properly now, because "garbage sampling in, garbage
spectrum out" is invisible until you compare two FFTs that should look
similar and don't.

### Batch, don't stream, raw samples

FFT needs a **window** of samples (e.g., 256, 512, or 1024 points), not a
continuous drip-feed. The standard pattern on the ESP32 side is:

1. Buffer N samples at a fixed rate into a local array
2. Once the buffer is full, send that whole window as one message
3. Start filling the next buffer (double-buffering avoids gaps)

This is a good moment to learn about **power-of-two window sizes** (256,
512, 1024...) — most FFT implementations (including ESP32-friendly ones like
`arduinoFFT` or `esp-dsp`) are radix-2, meaning they require or strongly
prefer power-of-two lengths for efficiency.

---

## 3. Transport: Why MQTT Fits This Problem

MQTT isn't just "a way to send data" — it's specifically a **publish/subscribe**
protocol built for many small, frequent messages from constrained devices,
which is exactly this project's shape. Worth understanding *why* it's the
standard choice here rather than just copying a code snippet:

- **Decoupling**: the ESP32 doesn't need to know the Pi's IP, how many
  subscribers exist, or whether they're online. It just publishes to a topic.
- **Lightweight**: minimal overhead compared to HTTP, which matters on a
  microcontroller with limited RAM/CPU.
- **QoS levels**: MQTT gives you a built-in vocabulary for "how much do I
  care if this message gets lost," which is a real design decision here (see below).

### Topic design is an API — treat it like one

A common beginner mistake is dumping everything into one topic like
`esp32/data`. Standard practice is a hierarchical, self-describing topic
structure, e.g.:

```
sensors/<device_id>/vibration/raw
sensors/<device_id>/vibration/fft
sensors/<device_id>/status
```

This pays off later: it lets you subscribe to just status for a health
dashboard, or just raw data for a specific machine, without filtering in code.

### QoS: pick it deliberately, not by default

- **QoS 0** (fire and forget): fine for high-frequency raw vibration windows
  where losing one window occasionally doesn't matter — you'll get another
  in a few seconds.
- **QoS 1** (at least once): better for derived/summary data (e.g., "fault
  detected" alerts) where you'd rather risk a duplicate than lose the message.

Thinking through *why* you'd choose one over the other is more valuable than
memorizing which QoS number to hardcode.

### Where should the FFT actually run?

This is a genuine architectural decision, not a solved problem — worth
reasoning through rather than defaulting to "obviously the Pi, it's more
powerful":

| Run FFT on... | Pros | Cons |
|---|---|---|
| ESP32 (edge) | Much less data over MQTT (spectrum, not raw samples); scales to more devices; Pi does less work | Limited RAM/CPU limits window size; harder to debug/iterate on the algorithm |
| Raspberry Pi (server) | Full Python ecosystem (NumPy/SciPy), easy to iterate and visualize; more compute for larger windows | More MQTT bandwidth (raw samples); Pi becomes a bottleneck as devices scale |

**Common standard approach for a learning/prototype project:** send raw
windows to the Pi first, get the FFT and fault detection working and
*validated* there where it's easy to inspect and plot, then consider pushing
FFT to the edge later as an optimization once you know what "good" looks
like. Don't optimize for scale before you've proven the concept.

---

## 4. Signal Processing: What the FFT Is Actually Telling You

It's worth being precise about what FFT gives you, because "run FFT on the
data" is not itself the fault-detection model — it's a *transformation* that
makes the fault-detection model's job easier.

### The core idea

A raw vibration signal in the time domain (acceleration vs. time) is hard to
interpret by eye — it looks like noisy squiggles. The FFT re-expresses that
same signal as a sum of sine waves at different frequencies, telling you
*how much energy exists at each frequency*. A worn belt tends to introduce
or amplify specific frequency components (e.g., at the belt-pass frequency
and its harmonics) that a healthy belt doesn't have, or has much less of.

### Practical, standard steps — and why each exists

1. **Remove DC offset / mean** before FFT. The MPU6050 accelerometer reads
   ~1g due to gravity even when perfectly still; that constant offset shows
   up as a large, meaningless spike at 0 Hz that can dwarf everything else
   if you don't subtract the mean first.

2. **Apply a window function** (Hann/Hamming are standard defaults) to your
   sample buffer before FFT. This exists to combat **spectral leakage** —
   because your buffer is a finite chunk of a continuous signal, the FFT
   implicitly assumes the buffer repeats forever, and any discontinuity at
   the edges smears energy across nearby frequency bins. A window tapers the
   edges to near-zero, which reduces (not eliminates) that smearing.

3. **Convert bin index → Hz**, using `frequency = bin_index * sample_rate / N`.
   It's easy to plot an FFT and forget the x-axis isn't meaningful until you
   do this conversion.

4. **Only look at bins up to Nyquist** (sample_rate / 2) — the upper half of
   a real-valued FFT output is a mirror image and doesn't add information.

### From spectrum to "is this normal?" — the actual predictive model

This is where "predictive model" stops being FFT and becomes machine
learning / statistics, and it's worth learning as its own topic rather than
assuming FFT does this automatically:

- **Baseline first.** Capture spectra from the machine when the belt is
  known-healthy. This baseline is your ground truth for "normal."
- **Feature extraction**, not raw spectra, as input to a model. Standard
  features: energy in specific frequency bands, peak frequency and its
  amplitude, total spectral energy, spectral centroid. Reducing ~500 FFT
  bins to a handful of meaningful numbers is what makes a small model
  trainable and interpretable.
- **Start simple.** A common, credible progression:
  1. Threshold/anomaly detection on one or two features (e.g., "alert if
     energy at belt-pass frequency exceeds baseline + N standard deviations")
  2. Statistical anomaly detection (e.g., isolation forest, one-class SVM)
     if thresholds prove too brittle
  3. Supervised classification (healthy vs. worn) only once you have
     *labeled* examples of both — which for a belt-wear project usually
     means physically inducing/observing wear over time, not something to
     assume you'll have on day one.

Don't reach for a deep learning model here — the feature space is small and
well-understood, and classical ML (or even careful thresholding) is the
right tool, easier to explain, and far easier to debug when it's wrong.

---

## 5. Backend on the Raspberry Pi: What "Hold the Data" Should Mean

"A small backend server to hold the data" is doing a lot of work in one
sentence — it's worth unpacking into its standard components rather than
writing one monolithic script:

- **MQTT broker**: something has to run the broker itself (Mosquitto is the
  standard, lightweight choice) — this is separate from your application
  logic, which subscribes as a client.
- **Ingestion service**: subscribes to the MQTT topics, parses incoming
  messages, and writes them somewhere durable.
- **Storage**: for time-series sensor data, a general-purpose relational
  database works, but this is a textbook use case for a **time-series
  database** (e.g., InfluxDB, TimescaleDB) — worth learning why: they're
  optimized for exactly this write pattern (frequent, timestamped, mostly
  append-only) and make time-range queries and downsampling far easier than
  a generic SQL schema you'd hand-roll yourself.
- **API/dashboard layer**: a lightweight way to query and visualize what's
  stored (Grafana pairs naturally with InfluxDB and gets you a dashboard
  with no frontend code at all — worth trying before building a custom UI).

### A principle worth adopting early: store raw *and* derived data separately

Keep the raw vibration windows (or at least a sample of them) even after
computing FFT/features from them. You will want to revisit raw data once you
realize your feature extraction was missing something — this is normal and
expected in any signal-processing project, not a sign you did it wrong the
first time.

---

## 6. Suggested Project Structure

```
/firmware        # ESP32 code: sampling, buffering, MQTT publish
/broker           # Mosquitto config
/ingestion        # Python service: MQTT subscribe -> DB write
/analysis         # FFT + feature extraction + model (offline dev/notebooks)
/dashboard        # Grafana config or lightweight web UI
/docs             # baseline captures, frequency calculations, design notes
```

Keeping `/analysis` separate and notebook-friendly (Jupyter) is a deliberate
choice: signal processing and model development are exploratory by nature —
you'll want to plot, iterate, and compare spectra interactively, which is
awkward to do inside a running service. Develop the algorithm offline first,
then port the finalized logic into `/ingestion` once it's stable.

---

## 7. Suggested Learning Order

If you're building this to *learn* rather than just to ship, this order
tends to build understanding correctly, each step validated before you rely
on it in the next:

1. Get raw accelerometer data off the MPU6050 and print it — verify it looks
   sane (gravity ~1g on one axis at rest) before anything else.
2. Get fixed-rate buffered sampling working; verify actual vs. intended
   sample rate.
3. Get MQTT publish/subscribe working with dummy data before wiring in the
   sensor, so you're not debugging two unfamiliar systems at once.
4. Capture a real baseline spectrum from a healthy machine; plot it; sanity
   check it against expected mechanical frequencies.
5. Only then: induce or find a way to observe belt wear, capture that
   spectrum, and start comparing.

Each of these is independently testable — resist the urge to wire the whole
pipeline together before any one piece is verified in isolation.

---

## 8. Key Terms Worth Understanding (Not Just Using)

- **Nyquist frequency** — the theoretical ceiling on what frequencies your
  sample rate can represent
- **Aliasing** — what happens when you violate Nyquist: high frequencies
  masquerade as false low frequencies
- **Spectral leakage** — smearing in the FFT output caused by analyzing a
  finite window of a signal
- **QoS (MQTT)** — the delivery guarantee tradeoff between reliability and
  overhead
- **Feature extraction** — reducing a spectrum to a small set of meaningful
  numbers for a model to consume
- **Baseline** — the reference "healthy" state everything else is measured against