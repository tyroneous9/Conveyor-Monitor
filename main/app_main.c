/*
 * Conveyor Monitor — WiFi + MQTT sensor publisher (simplified learning version)
 *
 * What this does:
 *   1. Connects to WiFi (SSID/password set via `idf.py menuconfig`)
 *   2. Connects to a plain (non-TLS) MQTT broker
 *   3. Samples the MPU6050 accelerometer at a fixed rate (esp_timer, not the
 *      FreeRTOS tick -- see the comment on sample_timer) into a small pool
 *      of window buffers, handed off to a separate publish task over a pair
 *      of FreeRTOS queues (see the comment on free_buffer_queue), which publishes
 *      each full window as one JSON message to
 *      sensors/<device_id>/vibration/raw. See backend/ingest.py for the
 *      consumer side of this exact contract.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "esp_crt_bundle.h"
#include "mpu6050.h"
#include "mqtt_client.h"
#include "protocol_examples_common.h"

static const char *TAG = "conveyor_monitor";

#define SAMPLE_RATE_HZ CONFIG_SAMPLE_RATE_HZ
#define WINDOW_SIZE CONFIG_SAMPLE_WINDOW_SIZE

/* Generous per-value budget (sign, 4 decimals, comma) so this always fits
 * whatever WINDOW_SIZE is configured to, instead of a fixed guess that could
 * silently become too small if WINDOW_SIZE changes. */
#define JSON_BUFFER_SIZE (WINDOW_SIZE * 3 * 20 + 128)

typedef struct {
    float ax[WINDOW_SIZE];
    float ay[WINDOW_SIZE];
    float az[WINDOW_SIZE];
} sample_window_t;

static esp_mqtt_client_handle_t mqtt_client;
static volatile bool mqtt_is_connected;
static mpu6050_handle_t mpu6050_sensor;
static char device_topic[64];
static esp_timer_handle_t sample_timer;
static char window_json_buf[JSON_BUFFER_SIZE];

/* A WINDOW_QUEUE_DEPTH-buffer pool, checked in and out via two FreeRTOS
 * queues, so a slow MQTT publish (network I/O, in publish_task) never blocks
 * or delays the next sample due (in sample_timer_cb):
 *   - free_buffer_queue holds indices of buffers safe to fill. sample_timer_cb
 *     checks one out to fill and, once full, hands its index to
 *     ready_buffer_queue.
 *   - publish_task blocks on ready_buffer_queue, publishes the window, then
 *     returns the index to free_buffer_queue.
 * If free_buffer_queue is ever empty, publish_task has fallen behind by a full
 * window -- sample_timer_cb drops the sample and logs it rather than
 * overwriting a buffer publish_task might still be reading. */
#define WINDOW_QUEUE_DEPTH 2
static sample_window_t window_pool[WINDOW_QUEUE_DEPTH];
static QueueHandle_t free_buffer_queue;
static QueueHandle_t ready_buffer_queue;

/* MQTT client event callback, registered in mqtt_app_start. Just tracks
 * connection state (mqtt_is_connected, read by publish_task) and logs --
 * publishing itself doesn't wait for this, since QoS 1 + the outbox handle
 * buffering while disconnected. */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to MQTT broker");
        mqtt_is_connected = true;
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from MQTT broker");
        mqtt_is_connected = false;
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "Publish acknowledged, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;
    default:
        break;
    }
}

/* Bounds how much a network drop can queue up in the client's outbox before
 * windows start getting dropped -- enough to ride out a ~10s hotspot hiccup
 * at the default sample rate/window size without growing unbounded on a
 * memory-constrained device. */
#define OUTBOX_LIMIT_BYTES (JSON_BUFFER_SIZE * 8)

static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_EXAMPLE_MQTT_BROKER_URI,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        /* Default buffer is sized for small example payloads, not a whole
         * JSON sample window -- match it to what we actually send. */
        .buffer.size = JSON_BUFFER_SIZE,
        /* Default is 120s; the network here is a phone hotspot, which can
         * idle-timeout/drop the radio to save battery -- keep traffic
         * frequent enough that it doesn't look idle (see TODO.md). The
         * client pings at roughly half this interval. */
        .session.keepalive = 30,
        /* QoS 1 publishes queue in this outbox and get resent on reconnect
         * (auto-reconnect is on by default) instead of being dropped the
         * moment the link blips -- see the outbox-full handling in
         * publish_task. */
        .outbox.limit = OUTBOX_LIMIT_BYTES,
        /* Default is 10s; reconnect quickly so a brief hotspot drop doesn't
         * let the outbox back up any longer than it has to. */
        .network.reconnect_timeout_ms = 2000,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

/* Fills device_topic with sensors/esp32-<last 3 MAC bytes>/vibration/raw,
 * so each device publishes to its own topic without any manual per-device
 * configuration. */
static void build_device_topic(void)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_read_mac failed: %s, using a placeholder device id", esp_err_to_name(err));
    }
    snprintf(device_topic, sizeof(device_topic),
             "sensors/esp32-%02x%02x%02x/vibration/raw", mac[3], mac[4], mac[5]);
}

/* Formats one more piece of text into `buf` at `offset` (printf-style).
 * Returns the new offset, or JSON_WRITE_FAILED if it wouldn't fit.
 */
#define JSON_WRITE_FAILED SIZE_MAX
static size_t json_write(char *buf, size_t buf_size, size_t offset, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buf + offset, buf_size - offset, fmt, args);
    va_end(args);

    if (written < 0 || (size_t)written >= buf_size - offset) {
        return JSON_WRITE_FAILED;
    }
    return offset + (size_t)written;
}

/* Helper function to append "<key>":[<v0>,<v1>,...] at *out_offset.
 * Returns false if it would overflow buf_size AND ALSO does not update *out_offset. */
static bool append_float_array(char *buf, size_t buf_size, size_t *out_offset,
                                const char *key, const float *values, int count)
{
    // Write the key and opening bracket, e.x. "ax":[
    size_t offset = json_write(buf, buf_size, *out_offset, "\"%s\":[", key);
    if (offset == JSON_WRITE_FAILED) return false;

    // Write each value, comma-separated, e.x. 1,2,3
    for (int i = 0; i < count; i++) {
        offset = json_write(buf, buf_size, offset, i == 0 ? "%.4f" : ",%.4f", values[i]);
        if (offset == JSON_WRITE_FAILED) return false;
    }

    // Write the closing bracket, e.g. ]
    offset = json_write(buf, buf_size, offset, "]");
    if (offset == JSON_WRITE_FAILED) return false;

    // Success: update the caller's offset and return true
    *out_offset = offset;
    return true;
}

/* Serializes one full window to the JSON contract documented at the top of
 * this file (and consumed by backend/ingest.py):
 *   {"sample_rate_hz":N,"ax":[...],"ay":[...],"az":[...]}
 * Returns false (leaving *out_len untouched) if it wouldn't fit in buf. */
static bool build_window_json(const sample_window_t *window, char *buf, size_t buf_size, size_t *out_len)
{
    size_t offset = json_write(buf, buf_size, 0, "{\"sample_rate_hz\":%d,", SAMPLE_RATE_HZ);
    if (offset == JSON_WRITE_FAILED) return false;

    if (!append_float_array(buf, buf_size, &offset, "ax", window->ax, WINDOW_SIZE)) return false;

    offset = json_write(buf, buf_size, offset, ",");
    if (offset == JSON_WRITE_FAILED) return false;

    if (!append_float_array(buf, buf_size, &offset, "ay", window->ay, WINDOW_SIZE)) return false;

    offset = json_write(buf, buf_size, offset, ",");
    if (offset == JSON_WRITE_FAILED) return false;

    if (!append_float_array(buf, buf_size, &offset, "az", window->az, WINDOW_SIZE)) return false;

    offset = json_write(buf, buf_size, offset, "}");
    if (offset == JSON_WRITE_FAILED) return false;

    *out_len = offset;
    return true;
}

/* esp_mqtt_client_publish()'s documented (but unnamed, in the library itself)
 * return value meaning "the outbox is full" -- see mqtt_client.h. */
#define MQTT_PUBLISH_OUTBOX_FULL (-2)

/* Consumer side of the free/ready queue pair described above free_buffer_queue:
 * blocks until sample_timer_cb hands off a full window, turns it into JSON,
 * publishes it over MQTT, then returns the buffer to the free pool. Runs as
 * its own task so a slow publish never delays the next sample. */
static void publish_task(void *arg)
{
    (void)arg;
    int ready_window_index;

    while (1) {

        // Block until a window is in the ready queue
        if (xQueueReceive(ready_buffer_queue, &ready_window_index, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // Convert the window to JSON, dropping it IF too large (unexpected values), THEN return buffer to free pool early
        size_t len;
        if (!build_window_json(&window_pool[ready_window_index], window_json_buf, sizeof(window_json_buf), &len)) {
            ESP_LOGE(TAG, "Window JSON exceeded %d-byte buffer, dropping window", JSON_BUFFER_SIZE);
            xQueueSend(free_buffer_queue, &ready_window_index, 0);
            continue;
        }

        /* Publish unconditionally, even while mqtt_is_connected is false: at
         * QoS 1 the client queues into its outbox (bounded by
         * OUTBOX_LIMIT_BYTES above) and flushes it on reconnect, so a brief
         * drop no longer means a silently lost window. */
        int msg_id = esp_mqtt_client_publish(mqtt_client, device_topic, window_json_buf, (int)len, /*qos=*/1, /*retain=*/0);
        if (msg_id == MQTT_PUBLISH_OUTBOX_FULL) {
            ESP_LOGW(TAG, "Outbox full, dropping window (broker unreachable too long)");
        } else if (!mqtt_is_connected) {
            ESP_LOGI(TAG, "Queued %d-sample window for %s while disconnected (outbox=%d bytes)",
                     WINDOW_SIZE, device_topic, esp_mqtt_client_get_outbox_size(mqtt_client));
        } else {
            ESP_LOGI(TAG, "Published %d-sample window to %s (%d bytes)", WINDOW_SIZE, device_topic, (int)len);
        }

        // Return buffer to free pool so sample_timer_cb can check it out again
        xQueueSend(free_buffer_queue, &ready_window_index, 0);
    }
}

/* Samples accelerometer data once into a window buffer from free_buffer_queue, handing it off to ready_buffer_queue once full (window filled).
 * Skips sampling when no free buffer is available (publish_task has fallen behind).
*/
static void sample_timer_cb(void *arg)
{
    (void)arg;

    // Index of pool buffer this window is filling, or -1 when none (start of a new window).
    static int active_window_index = -1;
    // Index of next sample to write into the active buffer, reset to 0 for new window.
    static int next_sample_index;

    if (active_window_index < 0) {
        if (xQueueReceive(free_buffer_queue, &active_window_index, 0) != pdTRUE) {
            ESP_LOGW(TAG, "publish_task fell behind, dropping sample (no free window buffer)");
            return;
        }
        next_sample_index = 0;
    }

    mpu6050_measurements_t accel;
    esp_err_t err = mpu6050_read_accel(mpu6050_sensor, &accel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read MPU6050: %s", esp_err_to_name(err));
        return;
    }

    // Write the sample into the active buffer
    sample_window_t *buf = &window_pool[active_window_index];
    buf->ax[next_sample_index] = accel.accel_x;
    buf->ay[next_sample_index] = accel.accel_y;
    buf->az[next_sample_index] = accel.accel_z;
    next_sample_index++;

    // If the window is filled to max, send the buffer to the ready queue for publishing
    if (next_sample_index >= WINDOW_SIZE) {
        xQueueSend(ready_buffer_queue, &active_window_index, 0);
        active_window_index = -1;
    }
}

void app_main(void)
{
    /* ESP-IDF baseline: NVS backs WiFi credential storage, esp_netif +
     * the default event loop are required by both WiFi and MQTT. */
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    build_device_topic();

    const mpu6050_config_t mpu6050_cfg = {
        .sda_io_num = CONFIG_MPU6050_SDA_GPIO,
        .scl_io_num = CONFIG_MPU6050_SCL_GPIO,
        .i2c_freq_hz = CONFIG_MPU6050_I2C_FREQ_HZ,
        .accel_fs = MPU6050_ACCEL_FS_4G,
    };
    ESP_ERROR_CHECK(mpu6050_init(&mpu6050_cfg, &mpu6050_sensor));

    /* Connects to WiFi using the SSID/password configured in
     * `idf.py menuconfig` under "Example Connection Configuration". */
    ESP_ERROR_CHECK(example_connect());

    mqtt_app_start();

    /* Seed the free queue with every buffer index so sample_timer_cb has a
     * pool to check out from as soon as sampling starts. */
    free_buffer_queue = xQueueCreate(WINDOW_QUEUE_DEPTH, sizeof(int));
    ready_buffer_queue = xQueueCreate(WINDOW_QUEUE_DEPTH, sizeof(int));
    configASSERT(free_buffer_queue != NULL && ready_buffer_queue != NULL);
    for (int i = 0; i < WINDOW_QUEUE_DEPTH; i++) {
        xQueueSend(free_buffer_queue, &i, 0);
    }

    xTaskCreate(publish_task, "publish_task", 4096, NULL, 5, NULL);

    /* Starts the periodic sampling timer last, only once WiFi/MQTT/the
     * publish task are all up, so sample_timer_cb never runs against
     * half-initialized state. */
    const esp_timer_create_args_t timer_args = {
        .callback = sample_timer_cb,
        .name = "sample_timer",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &sample_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(sample_timer, 1000000 / SAMPLE_RATE_HZ));
}
