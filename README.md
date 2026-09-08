# Conveyor Monitor

Fault classifier for an industrial conveyor belt: an ESP32 samples vibration off an MPU6050 accelerometer, streams it over MQTT to a Raspberry Pi, and analyzes this data using FFT to predict belt wear.

![Physical setup](analysis/figures/physical_setup.png)

## Hardware

**ESP32:** built-in WiFi, well documented and supported with a vendor framework (ESP-IDF), available as both a dev board (used here) and a bare chip for eventual production use, and cheap in bulk.

**MPU6050:** the ESP32 has mature I2C drivers for it and the documentation shows how to use it directly, its sample rate is adequate for the target vibration frequencies, and it's cheap.

**Raspberry Pi 5:** acts as the central device that receives and stores data from every ESP32 on the line. In production deployment this needs to be low-power and physically close to the sensors it's collecting from. A cheaper device could have been used, but the Pi 5 is what I already had.

**Mounting:** the circuit is mounted to the conveyor's metal frame near the motor, attached by magnets and tape.

## Architecture

```mermaid
flowchart LR
    MPU["MPU6050<br/>accelerometer"] -->|I2C| ESP["ESP32 firmware"]
    ESP -->|"publish window"| Broker["MQTT broker"]
    Broker --> Ingest["ingest.py"]
    Ingest -->|raw_windows| DB[("SQLite")]
    DB -->|unanalyzed windows| Analyze["analyze_fft.py"]
    Analyze -->|fft_results| DB
    Labels["labels.py<br/>(operator session ranges)"] -->|window_labels| DB
    DB -->|fft_results + window_labels| Classify["classify_faults.py"]
    Classify -->|baselines, classifications| DB
```

**Explanation:**

1. The MPU6050 measures vibration along three axes (x, y, z). This data is sampled by the ESP32 at an exact 500Hz. Samples are batched into windows, which are published as JSON to Mosquitto, a MQTT broker.

    - A sample is one accelerometer reading: one instance of `(x, y, z)`. The ESP32 takes one every 2ms (500Hz).

    - A window is a batch of 256 consecutive samples (~0.512 seconds), bundled together and sent as one JSON/MQTT message.

2. A Raspberry Pi hosts the broker locally, and it also reads the vibration data via `ingest.py` which subscribes to the published topic.

3. `ingest.py` validates each window and writes it into a SQLite database in the `raw_windows` table.

4. `analyze_fft.py` is used to analyze the data given enough windows. It performs FFT and writes the results into `fft_results`.

5. `analysis/labels.py` labels each recorded window healthy or worn from operator-recorded session time ranges (the belt is moved between a known-healthy and known-worn setup between recording sessions, so ground truth comes from which session's time range a window falls in). Labels are written to the `window_labels` table and must be populated before classification can run.

6. `classify_faults.py` reads the FFT results and labels, and classifies any new window healthy or worn by comparing the vibration against a baseline from known-healthy windows. Results get saved to the database (`baselines` and `classifications` tables).

7. `analysis/explore_spectra.ipynb` is a Jupyter notebook for checking the data manually.

## Repo layout

```
main/            ESP-IDF firmware: interrupt-driven sampling (MPU6050 DATA_RDY + FIFO), window buffering, MQTT publish
components/      MPU6050 I2C driver + vendored esp-mqtt / ethernet_init
backend/         ingest.py, analyze_fft.py, storage.py (SQLite schema)
analysis/        labels.py, the classifier, report figures, Notebook
deploy/          Mosquitto config + systemd unit for running the broker and ingest.py as persistent services on the Pi
```

## Analysis results

I ran `backend/analyze_fft.py` on 1,075 windows (429 healthy, 646 worn). These results are then plotted with `analysis/generate_figures.py`.

![Frequency spectrum: healthy vs. worn belt](analysis/figures/spectrum_comparison.png)

The worn belt shows an obvious peak at its belt-pass frequency (11.7Hz) while the motor's own rotation frequency (27.3Hz) barely changes between conditions.

![Full frequency spectrum, all axes, healthy vs. worn](analysis/figures/spectrum_full.png)

The same comparison across the full 0Hz-to-Nyquist range and all three axes (log amplitude), showing the belt-pass difference isn't an artifact of the 100Hz cutoff or the single axis (`ay`) used above.

![Raw time-domain signal, healthy vs. worn](analysis/figures/waveform_comparison.png)

![Repeatability across independent windows](analysis/figures/repeatability.png)

| Metric | Healthy (n=429) | Worn (n=646) | Mann-Whitney U |
|---|---|---|---|
| Peak frequency (Hz) | 32.73 ± 30.44 | 10.95 ± 12.50 | U=248583, p=5.22×10⁻¹¹⁴ |
| Peak amplitude (g) | 3.22 ± 4.04 | 17.52 ± 7.94 | U=17890, p=1.82×10⁻¹²⁹ |

Mann-Whitney U (a statistical test for data that isn't normally distributed) was used because peak frequency clusters into a handful of discrete FFT bins. The tiny p-values mean the difference between the healthy and worn data is unlikely to be random.

## Fault classification

To see whether any single given window can be classified as healthy or worn, a classifier is used: `analysis/classify_faults.py`. It does this by summing up the FFT amplitude and compares to a baseline.
Every baseline and prediction also gets saved to the `baselines` / `classifications` tables.

![Threshold classification: belt-pass band amplitude vs. baseline](analysis/figures/classification.png)

| | Predicted healthy | Predicted worn |
|---|---|---|
| **True healthy** | 125 | 4 |
| **True worn** | 219 | 427 |

71.2% accuracy on the 775 evaluated windows: 99.1% precision, but only 66.1% recall. Precision stays high (very few healthy windows get called worn), but a large share of worn windows (219 of 646) are missed as false negatives.

## Firmware design decisions

**1. Sampling uses interrupts and a double buffer:**
The first attempt at sampling was a simple loop with a delay (`vTaskDelay`), but the rate was off, which I confirmed directly with an oscilloscope. This board's FreeRTOS tick only runs at 100Hz, 10ms resolution. This means trying to sample at 500Hz (2ms) is impossible with such a delay, as it will be rounded up to 10ms minimum. In practice, sampling must be deferred to its own task.

The second attempt replaced the delay with `esp_timer`, a hardware timer independent of the FreeRTOS tick, reading samples in a dedicated, high-priority `sample_task` with a precise timer. That fixed the rate problem, but left two issues: first, the ESP32's timer period and the MPU6050's own internal output rate (`SMPLRT_DIV`) were two independently configured values with nothing forcing them to agree.  This coupling is a problem for scalability if sample rate were to change in the future. Second, even if the rate is precise, this independent task could still be stalled, which is explained in the next version.

The current version wires the MPU6050's INT pin to a GPIO and enables its DATA_RDY interrupt instead, so the sensor triggers the ESP32 exactly when a new sample exists, via a GPIO ISR (`mpu6050_int_isr_handler`) that wakes a dedicated `sample_task`. Additionally, the sensor's onboard FIFO now buffers samples to prevent data loss when the CPU is stalled.

Why is CPU stalled? `sample_task` can still occasionally be delayed by a few milliseconds. The WiFi/lwIP internals MQTT depends on run at a higher FreeRTOS priority than sample_task, and are able to interrupt it. Rather than ONLY reading the MPU6050's live accelerometer registers (which get overwritten by the next read, so a delay would drop samples), `sample_task` enables the sensor's onboard FIFO and reads everything currently buffered. A brief delay now costs latency, but not lost data.

**2. Sampling and MQTT share a double buffer:**

Successfully read samples are stored by queuing up in two window buffers. While one buffer is being filled with new samples, the other buffer (which already has a full window) is free to be turned into JSON and published to MQTT on a separate, concurrent task, so a slow network publish never delays the next sample.

**3. Windows instead of samples:**
By allowing the ESP32 to collect samples locally into a window first, this guarantees that any single window is a consecutive set of samples. A window in progress can never be truncated or corrupted by network drops, only by hardware issues.

MQTT can be configured via QoS (Quality of Service) to try to guarantee delivery of messages. In the case of a network drop, MQTT will keep retrying delivery, while windows that haven't been read yet will be enqueued into an outbox which can store up to 8 windows, all of which can be read once network is restored.

    - The outbox: a library feature that holds local messages in a queue before they actually deliver over the network.

The window size of 256 samples is specifically chosen for two reasons. First, there is a reasonable amount of time between each window (~0.512 seconds), which means that the 8 window outbox allows for approximately 4.1 seconds of network downtime before windows get dropped, causing data corruption. In practice, this worst case only happens if there is a serious outage in which case testing should be done some other time. Small, infrequent network drops are the main target of this protection time.

    - Size tradeoffs: Increasing the size of the outbox increases the RAM usage. It is a non-issue in this case because testing was done on a dev board, but for a standalone ESP32 chip, the RAM usage of a large outbox is non-trivial considering the size of each window.

**4. Locally hosted broker:**
My primary WiFi enforces WPA3-only auth, and this ESP32 doesn't reliably use WPA3. Public MQTT brokers are also slow from overload. The solution was to host a broker over my phone's hotspot.


## Vibration analysis design decisions

**1. Hann windowing before FFT:**
`analyze_fft.py` applies a Hann window (`np.hanning`) to each axis before taking the FFT. A raw 256-sample window is not an integer number of vibration cycles, so the edges at its start and end act like a discontinuity. Smoothing the edges with a Hann window trades a small amount of frequency resolution for a much cleaner spectrum.

**2. Band amplitude, not a single bin:**
`classify_faults.py` sums FFT magnitude over a frequency band (`band_amplitude`) rather than reading the amplitude of a single bin. At 500Hz over 256 samples, each FFT bin is ~1.95Hz wide, so the true peak can jitter into an adjacent bin between windows. Summing a band around the expected frequency absorbs jitter at the cost of some frequency precision.

**3. Classification uses only the `ay` axis:**
`analyze_fft.py` stores spectra for `ax`, `ay`, and `az` on every window, but `classify_faults.py`'s band amplitude only reads `fft_ay`. `ay` is the axis most aligned with the conveyor's direction of travel, where belt-pass vibration showed up most strongly. All three axes are still stored so they could be used for other fault detections later on.

**4. Threshold classification:**
A statistical threshold is sufficient for the current implementation, but ideally should be changed to a trained model as data becomes more varied and different classifications are needed.

**5. Windows are split chronologically:**
Adjacent windows are highly correlated (they're 0.512s apart from the same few minutes of recording), so both evaluation and fitting sets use chronologically split data.

**6. Session ranges are labeled manually:**
`labels.py` is used to manually label data as "healthy" or "worn" based on timeframe. This is the only way to determine known sets of data to create a baseline.

## Setup

**Firmware** (ESP-IDF):
```
idf.py set-target esp32
idf.py menuconfig   # set WiFi credentials and the MQTT broker URI (see main/Kconfig.projbuild)
idf.py build flash monitor
```

**Broker** (on the Pi):
```
sudo apt install mosquitto mosquitto-clients
sudo cp deploy/mosquitto/conveyor-monitor.conf /etc/mosquitto/conf.d/
sudo systemctl enable --now mosquitto
```
Listens on LAN: only safe on a private network (see `deploy/mosquitto/conveyor-monitor.conf`).

**Backend** (on the Pi):
```
cd backend
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
python3 ingest.py
python3 analyze_fft.py
```
`analyze_fft.py` processes whatever windows are not analyzed yet, with an optional limit.

**Analysis** (you have known set of healthy/worn data):
```
cd analysis
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
python3 labels.py \
    --healthy-range [datetime_start datetime_end] \
    --worn-range [datetime_start datetime_end]
python3 classify_faults.py
python3 generate_figures.py
```
`labels.py` must run first to classify a set of data as healthy or worn based on their datetime range.


## Current issues

**1. Classification limitations:**
A single window being classified as worn doesn't guarantee the belt is worn. It could've happened by chance or it may be a temporary fault. It is much more useful to see if there is a trend of worn windows which can be used to make more confident statements about belt wear.

Another concern is that belt is unlikely to be the only fault in the conveyor belt. Belt wear was specifically investigated in this project only because it is audibly obvious and is easily fixed after diagnosis. Other faults may have not caused down time so far, but it is a possibility in the future.

**2. No inference:**
Currently the classifier only validates known healthy or worn data. It is unable to do inferences on unknown data.

**3. PCB migration:**
The sensor circuit currently runs on a breadboard with an ESP32 dev board. This was good for prototyping, but installing it along multiple places along the conveyor belt is not cheap nor efficient. A PCB would be ideal to cut power use and make mounting more practical.