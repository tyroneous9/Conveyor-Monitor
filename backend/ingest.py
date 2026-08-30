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
DB_PATH = os.environ.get("FFT_DB_PATH", "fft_backend.sqlite3")
RAW_TOPIC_FILTER = "sensors/+/vibration/raw"
AXES = ("ax", "ay", "az")

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("ingest")


def validate_payload(payload):
    lengths = {len(payload[axis]) for axis in AXES}
    if len(lengths) != 1:
        raise ValueError(f"axis sample arrays have mismatched lengths: {lengths}")


def on_connect(client, userdata, flags, reason_code, properties=None):
    log.info("connected to %s:%d (reason=%s)", BROKER_HOST, BROKER_PORT, reason_code)
    client.subscribe(RAW_TOPIC_FILTER, qos=0)


def on_message(client, userdata, msg):
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

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.user_data_set(conn)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
    client.loop_forever()


if __name__ == "__main__":
    main()
