/*
 * Conveyor Monitor — WiFi + MQTT sensor publisher (simplified learning version)
 *
 * What this does:
 *   1. Connects to WiFi (SSID/password set via `idf.py menuconfig`)
 *   2. Connects to a plain (non-TLS) MQTT broker
 *   3. Samples the MPU6050 accelerometer, driven by the sensor's own DATA_RDY
 *      hardware interrupt (see mpu6050_int_isr_handler) rather than a
 *      software timer. Each wake drains whatever's currently sitting in the
 *      sensor's onboard FIFO (see sample_task) rather than just its live
 *      registers, so a late wake still recovers every sample that piled up
 *      in the meantime instead of losing all but the newest. Samples land
 *      in a small pool of window buffers, handed off to a separate publish
 *      task over a pair of FreeRTOS queues (see the comment on
 *      free_buffer_queue), which publishes each full window as one JSON
 *      message to sensors/vibration/raw. See backend/ingest.py for the
 *      consumer side of this exact contract.
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

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

#define VIBRATION_TOPIC "sensors/vibration/raw"

typedef struct {
    float ax[WINDOW_SIZE];
    float ay[WINDOW_SIZE];
    float az[WINDOW_SIZE];
} sample_window_t;

static esp_mqtt_client_handle_t mqtt_client;
static volatile bool mqtt_is_connected;
static mpu6050_handle_t mpu6050_sensor;
static TaskHandle_t sample_task_handle;
static char window_json_buf[JSON_BUFFER_SIZE];

/* A WINDOW_QUEUE_DEPTH-buffer pool, checked in and out via two FreeRTOS
 * queues, so a slow MQTT publish (network I/O, in publish_task) never blocks
 * or delays the next sample due (in sample_task):
 *   - free_buffer_queue holds indices of buffers safe to fill. sample_task
 *     checks one out to fill and, once full, hands its index to
 *     ready_buffer_queue.
 *   - publish_task blocks on ready_buffer_queue, publishes the window, then
 *     returns the index to free_buffer_queue.
 * If free_buffer_queue is ever empty, publish_task has fallen behind by a full
 * window -- sample_task drops the sample and logs it rather than
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
        /* Default buffer is sized for small example payloads, not a whole
         * JSON sample window -- match it to what we actually send. */
        .buffer.size = JSON_BUFFER_SIZE,
        /* Default is 120s; the network here is a phone hotspot, which can
         * idle-timeout/drop the radio to save battery -- keep traffic
         * frequent enough that it doesn't look idle. The client pings at
         * roughly half this interval. */
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
 * blocks until sample_task hands off a full window, turns it into JSON,
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

        // Publish unconditionally, and queue into outbox when connection is lost
        int msg_id = esp_mqtt_client_publish(mqtt_client, VIBRATION_TOPIC, window_json_buf, (int)len, /*qos=*/1, /*retain=*/0);
        if (msg_id == MQTT_PUBLISH_OUTBOX_FULL) {
            ESP_LOGW(TAG, "Outbox full, dropping window (broker unreachable too long)");
        } else if (!mqtt_is_connected) {
            ESP_LOGI(TAG, "Queued %d-sample window for %s while disconnected (outbox=%d bytes)",
                     WINDOW_SIZE, VIBRATION_TOPIC, esp_mqtt_client_get_outbox_size(mqtt_client));
        } else {
            ESP_LOGI(TAG, "Published %d-sample window to %s (%d bytes)", WINDOW_SIZE, VIBRATION_TOPIC, (int)len);
        }

        // Return buffer to free pool so sample_task can check it out again
        xQueueSend(free_buffer_queue, &ready_window_index, 0);
    }
}

/* GPIO ISR for the MPU6050's INT line: fires once per sensor sample
 * (DATA_RDY, enabled via mpu6050_enable_data_ready_interrupt). Runs in true
 * interrupt context, so it can't touch the I2C driver directly (I2C
 * transactions aren't ISR-safe) -- it just wakes sample_task via a task
 * notification, which is, and does the actual read there. */
static void IRAM_ATTR mpu6050_int_isr_handler(void *arg)
{
    (void)arg;
    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(sample_task_handle, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

/* Bounds the on-stack scratch array used to drain the MPU6050's FIFO below,
 * independent of how large a backlog piled up before this task got to run
 * (mpu6050_read_fifo_samples chunks its own I2C reads to the same limit, so
 * a bigger backlog just means more drain iterations, not a bigger buffer). */
#define FIFO_DRAIN_BATCH_SAMPLES 16

/* Blocks on the MPU6050's DATA_RDY interrupt (relayed via mpu6050_int_isr_handler),
 * then drains every accelerometer sample currently sitting in the sensor's
 * FIFO into a window buffer checked out from free_buffer_queue, handing it
 * off to ready_buffer_queue once full (window filled). Draining the FIFO
 * rather than reading one live register means a late wake (this task got
 * preempted past one or more DATA_RDY pulses) still recovers every sample
 * that piled up in the meantime, instead of only the newest one.
 * Skips sampling when no free buffer is available (publish_task has fallen behind).
*/
static void sample_task(void *arg)
{
    (void)arg;

    // Index of pool buffer this window is filling, or -1 when none (start of a new window).
    int active_window_index = -1;
    // Index of next sample to write into the active buffer, reset to 0 for new window.
    int next_sample_index = 0;

    while (1) {
        // Block until the sensor's INT line pulses for the next sample
        uint32_t pulses_since_last_wake = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (pulses_since_last_wake > 1) {
            ESP_LOGI(TAG, "sample_task woke late (%" PRIu32 " DATA_RDY pulses since last wake) -- draining FIFO",
                      pulses_since_last_wake);
        }

        while (1) {
            mpu6050_measurements_t batch[FIFO_DRAIN_BATCH_SAMPLES];
            int n_read;
            esp_err_t err = mpu6050_read_fifo_samples(mpu6050_sensor, batch, FIFO_DRAIN_BATCH_SAMPLES, &n_read);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to read MPU6050 FIFO: %s", esp_err_to_name(err));
                break;
            }
            if (n_read == 0) {
                break; // FIFO fully drained
            }

            for (int i = 0; i < n_read; i++) {
                if (active_window_index < 0) {
                    if (xQueueReceive(free_buffer_queue, &active_window_index, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "publish_task fell behind, dropping sample (no free window buffer)");
                        continue;
                    }
                    next_sample_index = 0;
                }

                // Write the sample into the active buffer
                sample_window_t *buf = &window_pool[active_window_index];
                buf->ax[next_sample_index] = batch[i].accel_x;
                buf->ay[next_sample_index] = batch[i].accel_y;
                buf->az[next_sample_index] = batch[i].accel_z;
                next_sample_index++;

                // If the window is filled to max, send the buffer to the ready queue for publishing
                if (next_sample_index >= WINDOW_SIZE) {
                    xQueueSend(ready_buffer_queue, &active_window_index, 0);
                    active_window_index = -1;
                }
            }
        }
    }
}

void app_main(void)
{
    /* ESP-IDF baseline: NVS backs WiFi credential storage, esp_netif +
     * the default event loop are required by both WiFi and MQTT. */
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    const mpu6050_config_t mpu6050_cfg = {
        .sda_io_num = CONFIG_MPU6050_SDA_GPIO,
        .scl_io_num = CONFIG_MPU6050_SCL_GPIO,
        .i2c_freq_hz = CONFIG_MPU6050_I2C_FREQ_HZ,
        .accel_fs = MPU6050_ACCEL_FS_4G,
    };
    ESP_ERROR_CHECK(mpu6050_init(&mpu6050_cfg, &mpu6050_sensor));
    ESP_ERROR_CHECK(mpu6050_enable_fifo(mpu6050_sensor));
    ESP_ERROR_CHECK(mpu6050_enable_data_ready_interrupt(mpu6050_sensor));

    /* Connects to WiFi using the SSID/password configured in
     * `idf.py menuconfig` under "Example Connection Configuration". */
    ESP_ERROR_CHECK(example_connect());

    mqtt_app_start();

    /* Seed the free queue with every buffer index so sample_task has a
     * pool to check out from as soon as sampling starts. */
    free_buffer_queue = xQueueCreate(WINDOW_QUEUE_DEPTH, sizeof(int));
    ready_buffer_queue = xQueueCreate(WINDOW_QUEUE_DEPTH, sizeof(int));
    configASSERT(free_buffer_queue != NULL && ready_buffer_queue != NULL);
    for (int i = 0; i < WINDOW_QUEUE_DEPTH; i++) {
        xQueueSend(free_buffer_queue, &i, 0);
    }

    xTaskCreate(publish_task, "publish_task", 4096, NULL, 5, NULL);
    xTaskCreate(sample_task, "sample_task", 4096, NULL, 6, &sample_task_handle);

    // Configure CONFIG_MPU6050_INT_GPIO as an interrupt pin
    const gpio_config_t int_gpio_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_MPU6050_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        // Set to interrupt on rising edge, since the MPU6050 INT pin is pulled high on sample ready
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&int_gpio_cfg));

    // Install the GPIO ISR service to watch for interrupts
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    // Register mpu6050_int_isr_handler to be called on interrupts from CONFIG_MPU6050_INT_GPIO
    ESP_ERROR_CHECK(gpio_isr_handler_add(CONFIG_MPU6050_INT_GPIO, mpu6050_int_isr_handler, NULL));
}
