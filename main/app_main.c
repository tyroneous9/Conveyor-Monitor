/*
 * Conveyor Monitor — WiFi + MQTT sensor publisher (simplified learning version)
 *
 * What this does:
 *   1. Connects to WiFi (SSID/password set via `idf.py menuconfig`)
 *   2. Connects to a plain (non-TLS) MQTT broker
 *   3. Samples the MPU6050 accelerometer at a fixed rate (esp_timer, not the
 *      FreeRTOS tick -- see the comment on s_sample_timer) into a small pool
 *      of capture buffers, handed off to a separate publish task over a pair
 *      of FreeRTOS queues (see the comment on s_free_queue), which publishes
 *      each full capture as one JSON message to
 *      sensors/<device_id>/vibration/raw. See backend/ingest.py for the
 *      consumer side of this exact contract.
 */

#include <stdbool.h>
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
#define CAPTURE_SIZE CONFIG_SAMPLE_CAPTURE_SIZE

/* Generous per-value budget (sign, 4 decimals, comma) so this always fits
 * whatever CAPTURE_SIZE is configured to, instead of a fixed guess that could
 * silently become too small if CAPTURE_SIZE changes. */
#define JSON_BUFFER_SIZE (CAPTURE_SIZE * 3 * 20 + 128)

typedef struct {
    float ax[CAPTURE_SIZE];
    float ay[CAPTURE_SIZE];
    float az[CAPTURE_SIZE];
} sample_capture_t;

static esp_mqtt_client_handle_t s_mqtt_client;
static volatile bool s_mqtt_connected;
static mpu6050_handle_t s_mpu6050;
static char s_device_topic[64];
static esp_timer_handle_t s_sample_timer;
static char s_json_buf[JSON_BUFFER_SIZE];

/* A CAPTURE_QUEUE_DEPTH-buffer pool, checked in and out via two FreeRTOS
 * queues, so a slow MQTT publish (network I/O, in publish_task) never blocks
 * or delays the next sample due (in sample_timer_cb):
 *   - s_free_queue holds indices of buffers safe to fill. sample_timer_cb
 *     checks one out to fill and, once full, hands its index to
 *     s_ready_queue.
 *   - publish_task blocks on s_ready_queue, publishes the capture, then
 *     returns the index to s_free_queue.
 * If s_free_queue is ever empty, publish_task has fallen behind by a full
 * capture -- sample_timer_cb drops the sample and logs it rather than
 * overwriting a buffer publish_task might still be reading. */
#define CAPTURE_QUEUE_DEPTH 2
static sample_capture_t s_captures[CAPTURE_QUEUE_DEPTH];
static QueueHandle_t s_free_queue;
static QueueHandle_t s_ready_queue;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to MQTT broker");
        s_mqtt_connected = true;
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from MQTT broker");
        s_mqtt_connected = false;
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
 * captures start getting dropped -- enough to ride out a ~10s hotspot hiccup
 * at the default sample rate/capture size without growing unbounded on a
 * memory-constrained device. */
#define OUTBOX_LIMIT_BYTES (JSON_BUFFER_SIZE * 8)

static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_EXAMPLE_MQTT_BROKER_URI,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        /* Default buffer is sized for small example payloads, not a whole
         * JSON sample capture -- match it to what we actually send. */
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

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);
}

static void build_device_topic(void)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_read_mac failed: %s, using a placeholder device id", esp_err_to_name(err));
    }
    snprintf(s_device_topic, sizeof(s_device_topic),
             "sensors/esp32-%02x%02x%02x/vibration/raw", mac[3], mac[4], mac[5]);
}

/* Appends "<key>":[<v0>,<v1>,...] at *poffset. Returns false (without
 * modifying *poffset) if it would overflow buf_size. */
#define APPEND(...) do { \
        int _n = snprintf(buf + offset, buf_size - offset, __VA_ARGS__); \
        if (_n < 0 || (size_t)_n >= buf_size - offset) { return false; } \
        offset += (size_t)_n; \
    } while (0)

static bool append_float_array(char *buf, size_t buf_size, size_t *poffset,
                                const char *key, const float *values, int n)
{
    size_t offset = *poffset;
    APPEND("\"%s\":[", key);
    for (int i = 0; i < n; i++) {
        APPEND(i == 0 ? "%.4f" : ",%.4f", values[i]);
    }
    APPEND("]");
    *poffset = offset;
    return true;
}

static bool build_capture_json(const sample_capture_t *cap, char *buf, size_t buf_size, size_t *out_len)
{
    size_t offset = 0;
    APPEND("{\"sample_rate_hz\":%d,", SAMPLE_RATE_HZ);
    if (!append_float_array(buf, buf_size, &offset, "ax", cap->ax, CAPTURE_SIZE)) return false;
    APPEND(",");
    if (!append_float_array(buf, buf_size, &offset, "ay", cap->ay, CAPTURE_SIZE)) return false;
    APPEND(",");
    if (!append_float_array(buf, buf_size, &offset, "az", cap->az, CAPTURE_SIZE)) return false;
    APPEND("}");
    *out_len = offset;
    return true;
}

#undef APPEND

static void publish_task(void *arg)
{
    (void)arg;
    int ready_buf;

    while (1) {
        if (xQueueReceive(s_ready_queue, &ready_buf, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        size_t len;
        if (!build_capture_json(&s_captures[ready_buf], s_json_buf, sizeof(s_json_buf), &len)) {
            ESP_LOGE(TAG, "Capture JSON exceeded %d-byte buffer, dropping capture", JSON_BUFFER_SIZE);
            xQueueSend(s_free_queue, &ready_buf, 0);
            continue;
        }

        /* Publish unconditionally, even while s_mqtt_connected is false: at
         * QoS 1 the client queues into its outbox (bounded by
         * OUTBOX_LIMIT_BYTES above) and flushes it on reconnect, so a brief
         * drop no longer means a silently lost capture. */
        int msg_id = esp_mqtt_client_publish(s_mqtt_client, s_device_topic, s_json_buf, (int)len, /*qos=*/1, /*retain=*/0);
        if (msg_id == -2) {
            ESP_LOGW(TAG, "Outbox full, dropping capture (broker unreachable too long)");
        } else if (!s_mqtt_connected) {
            ESP_LOGI(TAG, "Queued %d-sample capture for %s while disconnected (outbox=%d bytes)",
                     CAPTURE_SIZE, s_device_topic, esp_mqtt_client_get_outbox_size(s_mqtt_client));
        } else {
            ESP_LOGI(TAG, "Published %d-sample capture to %s (%d bytes)", CAPTURE_SIZE, s_device_topic, (int)len);
        }

        xQueueSend(s_free_queue, &ready_buf, 0);
    }
}

/* Runs in the esp_timer task at a fixed SAMPLE_RATE_HZ, independent of the
 * FreeRTOS tick rate (CONFIG_FREERTOS_HZ=100 here, i.e. 10ms resolution --
 * far too coarse for e.g. a 500Hz/2ms sample period). Kept minimal (one I2C
 * read, one buffer write) so it doesn't fall behind its own schedule; the
 * slow part (JSON + network publish) happens in publish_task instead. */
static void sample_timer_cb(void *arg)
{
    (void)arg;
    /* Which pool buffer this capture is filling, or -1 between captures.
     * Static: this callback only ever runs from the single esp_timer task,
     * one invocation at a time, so there's no concurrent access to guard
     * against. */
    static int s_active_buf = -1;
    static int s_fill_index;

    if (s_active_buf < 0) {
        if (xQueueReceive(s_free_queue, &s_active_buf, 0) != pdTRUE) {
            ESP_LOGW(TAG, "publish_task fell behind, dropping sample (no free capture buffer)");
            return;
        }
        s_fill_index = 0;
    }

    mpu6050_measurements_t accel;
    esp_err_t err = mpu6050_read_accel(s_mpu6050, &accel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read MPU6050: %s", esp_err_to_name(err));
        return;
    }

    sample_capture_t *buf = &s_captures[s_active_buf];
    buf->ax[s_fill_index] = accel.accel_x;
    buf->ay[s_fill_index] = accel.accel_y;
    buf->az[s_fill_index] = accel.accel_z;
    s_fill_index++;

    if (s_fill_index >= CAPTURE_SIZE) {
        xQueueSend(s_ready_queue, &s_active_buf, 0);
        s_active_buf = -1;
    }
}

void app_main(void)
{
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
    ESP_ERROR_CHECK(mpu6050_init(&mpu6050_cfg, &s_mpu6050));

    /* Connects to WiFi using the SSID/password configured in
     * `idf.py menuconfig` under "Example Connection Configuration". */
    ESP_ERROR_CHECK(example_connect());

    mqtt_app_start();

    s_free_queue = xQueueCreate(CAPTURE_QUEUE_DEPTH, sizeof(int));
    s_ready_queue = xQueueCreate(CAPTURE_QUEUE_DEPTH, sizeof(int));
    configASSERT(s_free_queue != NULL && s_ready_queue != NULL);
    for (int i = 0; i < CAPTURE_QUEUE_DEPTH; i++) {
        xQueueSend(s_free_queue, &i, 0);
    }

    xTaskCreate(publish_task, "publish_task", 4096, NULL, 5, NULL);

    const esp_timer_create_args_t timer_args = {
        .callback = sample_timer_cb,
        .name = "sample_timer",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_sample_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_sample_timer, 1000000 / SAMPLE_RATE_HZ));
}
