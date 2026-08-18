| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-P4 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | --------- | -------- | -------- | -------- | -------- |

# Conveyor Monitor — ESP32 WiFi + MQTT sensor publisher

A minimal example: the ESP32 connects to WiFi, connects to an MQTT broker,
and publishes a sensor reading every 5 seconds. There's no real sensor wired
up yet — it publishes a simulated, slowly-drifting temperature value so you
can learn the WiFi → MQTT flow before adding hardware.

## What's in this example

- `main/app_main.c` — connects to WiFi, connects to MQTT, publishes on a timer
- Plain MQTT (no TLS/certificates) on port 1883, to keep the code short while learning
- Publishes to the topic `conveyor/sensor/temperature`

### Hardware required

Just an ESP32 board and WiFi. No sensor required yet.

### Configure the project

```
idf.py menuconfig
```

- Under **Example Connection Configuration**, set your WiFi SSID and password.
- Under **Example Configuration**, set the MQTT broker URI (defaults to the
  public `mqtt://test.mosquitto.org:1883`).

### Build, flash, and monitor

```
idf.py -p PORT flash monitor
```

(To exit the serial monitor, type `Ctrl-]`.)

### Watching the data arrive

From another device (your laptop, a Raspberry Pi, etc.) with
[mosquitto-clients](https://mosquitto.org/download/) installed:

```
mosquitto_sub -h test.mosquitto.org -t conveyor/sensor/temperature
```

You should see a new number print every ~5 seconds as the ESP32 publishes.

## Example Output

```
I (3714) event: sta ip: 192.168.0.139, mask: 255.255.255.0, gw: 192.168.0.2
I (4164) conveyor_monitor: Connected to MQTT broker
I (9174) conveyor_monitor: Published conveyor/sensor/temperature = 22.4
I (9274) conveyor_monitor: Publish acknowledged, msg_id=12345
I (14184) conveyor_monitor: Published conveyor/sensor/temperature = 21.8
```
