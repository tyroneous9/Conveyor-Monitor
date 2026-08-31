# TODO

## Networking / MQTT

- [x] Network topology decided: the iPhone hotspot (`wesley-iphone`) is the
      source of truth for this project. ESP32, Pi, and PC all join that same
      hotspot WiFi network — no home router (sidesteps its WPA3-only issue),
      no wired link to the PC (this ESP32 has no native USB networking or
      Ethernet PHY, so that path was never viable anyway).
- [ ] Run Mosquitto locally on a device already on the hotspot (Pi or PC) and
      point `CONFIG_EXAMPLE_MQTT_BROKER_URI` at its hotspot-assigned LAN IP,
      e.g. `mqtt://<lan-ip>:1883`. This is the actual fix for the earlier
      connection failures: traffic between hotspot-joined devices stays on
      the phone's local segment and never has to cross the carrier's WAN, so
      the carrier-side port/DPI blocking that broke `test.mosquitto.org`
      never comes into play.
- [ ] Once the local broker is confirmed working, retire the
      `wss://broker.hivemq.com:8884/mqtt` public-cloud config
      ([sdkconfig:711](sdkconfig#L711)) — it was a workaround for reaching a
      broker over the WAN, which a local broker makes unnecessary.
- [x] Phone hotspots can idle-timeout or drop the radio to save battery/data
      after a period of inactivity — MQTT keepalive shortened from the 120s
      default to 30s (`.session.keepalive` in
      [app_main.c:99](main/app_main.c#L99)) so the client pings roughly every
      15s, well inside typical hotspot idle-timeout windows. Still worth
      watching the logs for reconnects during long test runs once this is
      running on real hardware.
- [x] Topics redesigned per README §3: firmware now publishes to
      `sensors/esp32-<mac>/vibration/raw` ([app_main.c:104-113](main/app_main.c#L104-L113)),
      device id derived from the station MAC via `esp_read_mac`, matching
      what `backend/ingest.py` subscribes to (`sensors/+/vibration/raw`).
      Retired the old flat `conveyor/sensor/accel` topic.
- [x] QoS fixed end-to-end for raw windows: firmware publishes at QoS 1
      ([app_main.c:172](main/app_main.c#L172)) and `ingest.py` now subscribes
      at QoS 1 too, with a stable `client_id` and `clean_session=False`
      ([ingest.py](backend/ingest.py)) — effective MQTT delivery is
      `min(publish qos, subscribe qos)`, so the previous QoS 0 subscribe was
      silently downgrading every window to at-most-once regardless of the
      firmware's setting, and the default clean session meant nothing was
      held for redelivery across a reconnect. `.../status` topic and
      derived-alert QoS choices are still open — no status/alert messages
      exist yet.

## Sensing / sampling

- [x] Fixed-rate, windowed, double-buffered sampling implemented
      ([app_main.c](main/app_main.c)): an `esp_timer` (not the FreeRTOS tick
      — `CONFIG_FREERTOS_HZ=100` here is only 10ms resolution, too coarse for
      e.g. a 500Hz/2ms period, and a naive `vTaskDelay` loop would've rounded
      the period down to 0 ticks and sampled uncontrolled) fires
      `sample_timer_cb` at `CONFIG_SAMPLE_RATE_HZ` (default 500), filling one
      of two alternating window buffers of `CONFIG_SAMPLE_WINDOW_SIZE`
      samples (default 256, power-of-two per README §2). A full window hands
      off via task notification to `publish_task`, which builds the JSON and
      publishes — kept out of the timer callback so a slow MQTT publish never
      delays the next sample. Both rate and window size are Kconfig options.
      MQTT client buffer size (`.buffer.size`) is sized from
      `JSON_BUFFER_SIZE`, computed from `WINDOW_SIZE` so it can't silently
      fall out of sync with it.
- [ ] Work out target mechanical frequencies (motor RPM, belt-pass frequency)
      to size `CONFIG_SAMPLE_RATE_HZ` against Nyquist — still not done; 500Hz
      is a placeholder default, not derived from a measurement.
- [ ] Not yet run on real hardware — no ESP-IDF toolchain available in this
      environment to build/flash, so this is reviewed but not compile- or
      hardware-verified. First real build should double check
      `esp_timer_create`/`esp_timer_start_periodic` return values and watch
      the logs for "Window JSON exceeded ... buffer" (would indicate the
      `JSON_BUFFER_SIZE` formula's per-value byte budget was wrong) and for
      dropped-window warnings around MQTT reconnects.

## Raspberry Pi backend — minimal, FFT only

Split into two independent pieces on purpose: ingestion only ever writes raw
data, analysis only ever reads it back. Ingestion never blocks on (or fails
because of) analysis, and analysis can be re-run against history any time
without touching MQTT. No dashboard, no fault-detection model — those are
separate, later concerns and are explicitly deferred (see below). Depends on
the windowed/fixed-rate sampling work above — there's nothing to analyze
until the ESP32 is publishing whole windows instead of single (x,y,z) points.

- [ ] Mosquitto broker running on the Pi (or PC — see Networking section
      above); `ingest.py` is just another client on that broker.
- [x] Storage (`backend/storage.py`): SQLite, not Postgres — writes come
      from one script at a time, no separate daemon competing with Mosquitto
      on the Pi, ships in Python's stdlib. Two tables, `raw_windows` and
      `fft_results` (linked by `window_id`), per README §5's "keep raw and
      derived data separately." WAL mode enabled so `/analysis` notebooks
      can read the file concurrently. DB path via `FFT_DB_PATH` env var,
      defaulting to `fft_backend.sqlite3` **anchored to the backend/
      directory** (`os.path.dirname(os.path.abspath(__file__))`), not the
      process's cwd — `ingest.py` (long-running) and `analyze_fft.py`
      (periodic, e.g. cron) are separate processes that can easily be
      launched from different working directories, and a bare relative
      filename would've silently pointed them at two different files.
      Verified: running `analyze_fft.py` from `/tmp` with no `FFT_DB_PATH`
      set still correctly resolved to `backend/fft_backend.sqlite3`. File is
      gitignored.
- [x] Ingestion (`backend/ingest.py`): subscribes to
      `sensors/<device_id>/vibration/raw` at QoS 1 with a persistent session
      (`client_id="conveyor-ingest"`, `clean_session=False` — see Networking
      section above), validates the window payload
      (`{"sample_rate_hz", "ax", "ay", "az"}`, equal-length axes), and writes
      it straight to `raw_windows` — no FFT, no MQTT publish back out.
- [x] Analysis (`backend/analyze_fft.py`): a separate, standalone script —
      no MQTT client at all. Pulls raw windows with no matching `fft_results`
      row yet (`storage.fetch_unanalyzed_windows`), preprocesses each axis
      (subtract mean, Hann window per README §4), runs `numpy.fft.rfft`
      (bins are Hz-labeled via `rfftfreq` and naturally stop at Nyquist), and
      writes the spectrum + peak back to `fft_results`. Idempotent — already
      analyzed windows are skipped, so it's safe to run repeatedly. Supports
      `--watch SECONDS` to loop continuously instead of exiting after one
      pass, so "run it periodically" doesn't require external cron/systemd
      setup; verified a 1s-interval watch loop picks up a newly-seeded
      window on its first pass and correctly reports 0 on subsequent passes.
      `backend/requirements.txt` covers both scripts (paho-mqtt, numpy).
      Not yet run against real hardware/broker — no ESP-IDF toolchain or
      MQTT broker available in this environment; verified with synthetic
      data and mocked MQTT connections only (see Sensing/sampling section).
- [ ] Explicitly still out of scope: Grafana/dashboard, feature extraction,
      baseline comparison, fault detection. Those come later (see next
      section) once raw FFT output has been sanity-checked by eye against
      expected mechanical frequencies — the DB now makes that possible, but
      nothing reads `fft_results` yet.
- [x] Fake data generator (`backend/seed_fake_data.py`): dev/test utility,
      not part of the production ingest→analyze path. Writes synthetic
      windows straight into `raw_windows` via the same `storage.store_window`
      `ingest.py` uses (extended with an optional `received_at` override so
      a multi-window session can be backdated over simulated time instead of
      real sleeps), so downstream code needs no special-casing. Supports
      `--condition healthy|worn`: motor-fundamental content is the same in
      both, belt-pass-frequency content is what differs (small when healthy,
      boosted with harmonics when worn) — modeling README §4's actual
      description of what a worn belt does to a spectrum, not just "add
      noise." Verified end-to-end (seed → `analyze_fft.py` →
      `explore_spectra.ipynb`): healthy consistently peaks at the motor
      fundamental (~29.3Hz, amplitude ~3), worn consistently peaks at
      belt-pass frequency (~7.8Hz, amplitude ~20-24, >20x healthy) with
      harmonics visible in the spectrum plot, and the peak-over-time plot
      shows two clean, stable, separated device traces. (First pass gave
      healthy belt-pass content the same amplitude as the motor fundamental,
      which made the peak flicker between the two per-window instead of
      settling on the motor fundamental — fixed by making healthy's
      belt-pass amplitude clearly subordinate, as a real healthy belt's
      would be.)
- [x] Frequency jitter widened (motor 2%→5%, belt 2%→12%) and dataset scaled
      from 20 to 100 windows per condition (200 total). The old 2% jitter
      never moved either frequency far enough to cross an FFT bin boundary
      (1.95 Hz wide at 500Hz/256-sample defaults) — every window landed on
      the exact same bin, which is what produced the zero-variance data that
      broke the significance test below. Checked the actual bin-crossing
      math before picking new values (motor's true frequency sits close to
      a bin center, needs less jitter to cross; belt's sits more off-center,
      needs more) rather than guessing. Confirmed post-fix: healthy peak
      freq now 29.34±1.17Hz (was 29.30±0.00), worn 8.12±0.72Hz (was
      7.81±0.00) — real variance, condition separation still clean.

## Signal processing / inference

- [x] `/analysis` scaffolded (`analysis/explore_spectra.ipynb`,
      `analysis/requirements.txt`): reads `backend/fft_backend.sqlite3`,
      plots the most recent spectrum per device (the README §4 "sanity check
      against expected mechanical frequencies" step) and peak frequency over
      time per device (where baseline drift will eventually show up).
      Read-only — nothing here writes back to the database. Verified by
      executing it end-to-end (`jupyter nbconvert --execute`) against a
      synthetic multi-device, multi-window database; both plots rendered
      correctly. No feature extraction, baseline comparison, or
      threshold/anomaly detection logic yet — deliberately not building that
      blind, without real spectra to develop it against (README's own "don't
      optimize before you've proven the concept").
- [x] Static report figures (`analysis/generate_figures.py`): matplotlib +
      scipy, run against the real `backend/fft_backend.sqlite3`, output
      embedded directly in README.md (`analysis/figures/*.png` +
      `summary_table.md`). Deliberately plain PNGs, not an interactive page —
      an earlier interactive HTML/JS build (SVG charts, hover tooltips, a
      `docs/` GitHub Pages setup) was built, then removed at the user's
      request in favor of this. Caught by scipy itself on first run: peak
      frequency is quantized to an FFT bin, and with this dataset's small
      per-window jitter every window in a session lands on the *same* bin —
      zero variance in both groups, so `scipy.stats.ttest_ind` degenerates to
      `t=inf` with a precision-loss warning — a t-test is the wrong tool
      here, not just an inconvenient result on this data. First pass worked
      around it (detect zero variance, skip the test, report a plain
      sentence); real fix is `scipy.stats.mannwhitneyu` instead of a t-test
      for both rows — rank-based, no variance assumption, valid for the
      degenerate (frequency) and continuous (amplitude) case alike. Numbers
      below are from the 20-per-condition run this was first built and
      diagnosed against; after the jitter/scale-up fix above (100 per
      condition, real variance in both metrics), Mann-Whitney remains the
      right choice regardless — peak frequency is still bin-quantized
      (discrete, non-normal), just no longer literally zero-variance — and
      still finds complete separation: peak frequency U=10000, p=2.67×10⁻³⁸;
      peak amplitude U=0, p=2.56×10⁻³⁴ (current numbers, in README.md).
- [x] Threshold fault classifier (`analysis/classify_faults.py`): README §4
      step 1 of 3 ("threshold/anomaly detection on one or two features"),
      deliberately not steps 2/3 (statistical anomaly detection, supervised
      classification) — see the "classifier is a threshold, not a trained
      model" note in README.md's Design decisions for why going further
      against only-synthetic data would be actively counterproductive, not
      just premature.
      One feature: FFT amplitude summed over a band around the belt-pass
      frequency (a band, not a single bin — the frequency-jitter fix above
      means the true peak can land in an adjacent bin between windows, so a
      single-bin lookup would sometimes miss it entirely). Baseline = mean +
      3 std, fit on 70% of the healthy device's windows (capture order),
      held out the other 30% for evaluation — evaluating "does healthy data
      clear its own threshold" on the same windows used to set that
      threshold would be circular, so those windows are excluded from
      scoring even though they're still plotted for context.
      `backend/storage.py` gained two tables for this: `baselines` (durable,
      timestamped — a new row per computation, not an overwrite, so history
      isn't lost) and `classifications` (one row per window: feature value,
      threshold, predicted label). `analysis/classify_faults.py` is in
      `analysis/`, not `backend/`, following README §6's layout, so it
      imports `storage` via a `sys.path` insert rather than duplicating the
      write functions — a deliberate, minor deviation from
      `generate_figures.py`'s pattern (which duplicates simple read-only
      queries instead), justified because this script *writes* using a
      schema that needs to stay centrally defined, not read-only convenience
      queries cheap to duplicate.
      Bug caught before trusting the result: first run reported n=200 and
      100% accuracy, but 70 of those 200 were the baseline-fit windows
      themselves — scored against a threshold derived from their own values,
      which is circular, not a real test. Fixed by excluding
      `role=="baseline-fit"` from the scored/reported set; the honest number
      is 130 genuinely held-out/independent windows (30 healthy + 100 worn),
      still 100% accuracy.
      **Caveat that matters more than the accuracy number**: evaluated only
      against `backend/seed_sample_data.py`'s hand-picked synthetic model.
      100% accuracy here is evidence the threshold approach is sound *given
      that model* — it is not evidence it'll work on a real belt, which may
      not separate this cleanly, may drift with load/speed, or may show wear
      as a different feature entirely. Re-fitting and re-evaluating this
      same script against real captured data is the actual next step, not
      building a fancier classifier now.

## Project structure

- [x] `/analysis` now exists per README §6. `/broker`, `/ingestion`,
      `/dashboard`, `/docs` still don't — `/ingestion`'s role is currently
      filled by `backend/` (narrower scope than README's "ingestion service +
      DB + dashboard" bundle; see the backend section above for why). Full
      rename/restructure (e.g. moving the ESP-IDF project itself into
      `/firmware`) intentionally not done here: it'd touch build-system paths
      (`CMakeLists.txt`, `.vscode/c_cpp_properties.json`, `.clangd`) that
      can't be verified without the ESP-IDF toolchain this environment
      doesn't have, so a mistake there could leave the project unbuildable
      with no way to catch it. Worth doing once a real build is available to
      confirm against.
- [x] Housekeeping found while here: `backend/__pycache__/*.pyc` had been
      committed (including a stale one for the now-deleted `fft_service.py`)
      because `.gitignore` didn't exclude Python bytecode cache. Added
      `__pycache__/`, `*.pyc`, `.ipynb_checkpoints/` to `.gitignore` and
      `git rm --cached` the tracked `.pyc` files — staged, not committed.

## Hardware

- [ ] test vibration sensor
