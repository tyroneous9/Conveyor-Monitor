/*
 * Conveyor Monitor firmware
 *
 * Logic:
 *   1. Connects to WiFi (hardcoded SSID/password)
 *   2. Connects to MQTT broker
 *   3. Samples MPU6050
 *   4. Publishes sampled data to the MQTT broker as JSON
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

// Huge json buffer (can be reduced) in case of large window sizes
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

// Queue limit for the window buffer pool
#define WINDOW_QUEUE_DEPTH 2
static sample_window_t window_pool[WINDOW_QUEUE_DEPTH];
static QueueHandle_t free_buffer_queue;
static QueueHandle_t ready_buffer_queue;

// MQTT connection handler
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

// Outbox limit for MQTT client (how many windows can be stored while connection is down)
#define OUTBOX_LIMIT_BYTES (JSON_BUFFER_SIZE * 8)

static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_EXAMPLE_MQTT_BROKER_URI,
        .buffer.size = JSON_BUFFER_SIZE,
        .outbox.limit = OUTBOX_LIMIT_BYTES,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

/* Formats some text into array `buf` at index `offset`
 * Returns the new offset, or JSON_WRITE_FAILED if it won't fit */
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

/* Helper function to format key-value pairs as JSON, 
 * Specifically for vibration dimension (keys) to individual samples (values) 
 * e.x. "ax":[1.0000,2.0000,...] */
static bool append_float_array(char *buf, size_t buf_size, size_t *out_offset,
                                const char *key, const float *values, int count)
{
    // Write the key and opening bracket, e.x. "ax":[
    size_t offset = json_write(buf, buf_size, *out_offset, "\"%s\":[", key);
    if (offset == JSON_WRITE_FAILED) return false;

    // Write the values, comma-separated, e.x. 1,2,3
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

/* Writes a window as JSON into 'buf'
 * Returns false if it wouldn't fit in buf. */
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

// Return value for when the MQTT outbox is full
#define MQTT_PUBLISH_OUTBOX_FULL (-2)

// RTOS task for publishing windows over MQTT
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

// GPIO ISR handler for MPU6050's INT line
static void IRAM_ATTR mpu6050_int_isr_handler(void *arg)
{
    (void)arg;
    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(sample_task_handle, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

// Limit for samples read in one batch from the MPU6050's FIFO 
#define FIFO_DRAIN_BATCH_SAMPLES 16


// RTOS task for sampling the MPU6050 and filling window buffers
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
    // Initialize NVS, network interface, and default event loop
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

    // Connect to the network
    ESP_ERROR_CHECK(example_connect());

    // Start MQTT client
    mqtt_app_start();

    // Create the free and ready buffer queues
    free_buffer_queue = xQueueCreate(WINDOW_QUEUE_DEPTH, sizeof(int));
    ready_buffer_queue = xQueueCreate(WINDOW_QUEUE_DEPTH, sizeof(int));
    configASSERT(free_buffer_queue != NULL && ready_buffer_queue != NULL);
    for (int i = 0; i < WINDOW_QUEUE_DEPTH; i++) {
        xQueueSend(free_buffer_queue, &i, 0);
    }

    // Create the publish and sample tasks
    xTaskCreate(publish_task, "publish_task", 4096, NULL, 5, NULL);
    xTaskCreate(sample_task, "sample_task", 4096, NULL, 6, &sample_task_handle);

    // Configure the MPU6050 INT GPIO pin as an interrupt pin
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
