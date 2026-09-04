#!/usr/bin/env python3
"""MQTT ingestion for the Pi: writes raw accelerometer windows to SQLite.

Subscribes to sensors/<device_id>/vibration/raw. Each message is one
fixed-rate sample window from the ESP32, as JSON:

    {"sample_rate_hz": 500, "ax": [...], "ay": [...], "az": [...]}

(equal-length per-axis arrays). Each window is stored as-is in the
raw_windows table (see storage.py) -- nothing else happens here. FFT
analysis is a separate, on-demand script (analyze_fft.py) that reads from
this same database; keeping the two apart means ingestion never blocks on
(or fails because of) analysis, and analysis can be re-run against history
any time without re-touching MQTT.

Usage:
    pip install -r requirements.txt
    MQTT_BROKER_HOST=<broker-lan-ip> python3 ingest.py
(defaults to localhost:1883, i.e. running on the same device as the broker)
"""

import json
import logging
import os
import sqlite3

import paho.mqtt.client as mqtt

import storage

BROKER_HOST = os.environ.get("MQTT_BROKER_HOST", "localhost")
BROKER_PORT = int(os.environ.get("MQTT_BROKER_PORT", "1883"))
# Anchored to this script's own directory, not the process's cwd -- ingest.py
# and analyze_fft.py are meant to run as separate, independently-launched
# processes (e.g. one as a long-running service, the other from cron), and a
# bare relative filename would silently point them at two different files if
# they're started from different working directories.
DEFAULT_DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fft_backend.sqlite3")
DB_PATH = os.environ.get("FFT_DB_PATH", DEFAULT_DB_PATH)
# Stable, not the paho-generated random default: a persistent session (see
# clean_session=False in main()) is only useful if the broker recognizes the
# same client reconnecting.
CLIENT_ID = os.environ.get("MQTT_CLIENT_ID", "conveyor-ingest")
RAW_TOPIC_FILTER = "sensors/+/vibration/raw"
AXES = ("ax", "ay", "az")

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("ingest")


def validate_payload(payload):
    """Raise ValueError unless ax/ay/az are all present and the same length
    (a malformed or truncated window from the firmware)."""
    lengths = {len(payload[axis]) for axis in AXES}
    if len(lengths) != 1:
        raise ValueError(f"axis sample arrays have mismatched lengths: {lengths}")


def on_connect(client, userdata, flags, reason_code, properties=None):
    log.info("connected to %s:%d as %s (reason=%s)", BROKER_HOST, BROKER_PORT, CLIENT_ID, reason_code)
    # QoS 1 to match the firmware's publish QoS -- effective delivery is
    # min(publish qos, subscribe qos), so subscribing at 0 would silently
    # downgrade every window to at-most-once regardless of what the ESP32
    # sends. Combined with clean_session=False below, the broker holds
    # QoS>=1 messages published while this client is offline and redelivers
    # them on reconnect instead of dropping them.
    client.subscribe(RAW_TOPIC_FILTER, qos=1)


def on_message(client, userdata, msg):
    """Per-message MQTT callback: parse device_id out of the topic
    (sensors/<device_id>/vibration/raw), validate the JSON payload, and
    store it as one raw window. Any failure just logs and drops that one
    message -- a bad window from one device shouldn't stop the client's
    event loop or affect other devices."""
    parts = msg.topic.split("/")
    if len(parts) != 4:
        log.warning("ignoring message on unexpected topic: %s", msg.topic)
        return
    device_id = parts[1]

    try:
        payload = json.loads(msg.payload)
        validate_payload(payload)
    except (ValueError, KeyError, json.JSONDecodeError) as exc:
        log.warning("bad window from %s: %s", device_id, exc)
        return

    conn = userdata
    try:
        window_id = storage.store_window(conn, device_id, payload)
    except sqlite3.Error as exc:
        log.warning("failed to store window from %s: %s", device_id, exc)
        return

    log.info("device=%s stored window_id=%d n=%d", device_id, window_id, len(payload["ax"]))


def main():
    conn = storage.connect(DB_PATH)
    log.info("storing to %s", DB_PATH)

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=CLIENT_ID, clean_session=False)
    client.user_data_set(conn)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
    client.loop_forever()


if __name__ == "__main__":
    main()
