/*
 * Conveyor Monitor — WiFi + MQTT sensor publisher (simplified learning version)
 *
 * What this does:
 *   1. Connects to WiFi (SSID/password set via `idf.py menuconfig`)
 *   2. Connects to a plain (non-TLS) MQTT broker
 *   3. Every few seconds, publishes a simulated sensor reading
 *
 * There is no real sensor wired up yet — read_simulated_temperature() below
 * is a stand-in. Once you have real hardware, replace its body with an
 * actual sensor read (e.g. reading an ADC pin or an I2C sensor) and keep
 * everything else the same.
 */

#include <stdbool.h>
#include <stdio.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "esp_crt_bundle.h"
#include "mqtt_client.h"
#include "protocol_examples_common.h"

static const char *TAG = "conveyor_monitor";

#define SENSOR_TOPIC "conveyor/sensor/temperature"
#define PUBLISH_INTERVAL_MS 5000

static esp_mqtt_client_handle_t s_mqtt_client;
static volatile bool s_mqtt_connected;

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

static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_EXAMPLE_MQTT_BROKER_URI,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);
}

/* Placeholder for a real sensor read — swap this out once hardware is wired up. */
static float read_simulated_temperature(void)
{
    static float temperature = 22.0f;
    /* Drift by up to +/-1.0 degree each call so the value visibly changes. */
    temperature += ((float)(esp_random() % 21) - 10.0f) / 10.0f;
    return temperature;
}

static void sensor_publish_task(void *arg)
{
    (void)arg;
    char payload[16];

    while (1) {
        if (s_mqtt_connected) {
            int len = snprintf(payload, sizeof(payload), "%.1f", read_simulated_temperature());
            esp_mqtt_client_publish(s_mqtt_client, SENSOR_TOPIC, payload, len, /*qos=*/1, /*retain=*/0);
            ESP_LOGI(TAG, "Published %s = %s", SENSOR_TOPIC, payload);
        }
        vTaskDelay(pdMS_TO_TICKS(PUBLISH_INTERVAL_MS));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Connects to WiFi using the SSID/password configured in
     * `idf.py menuconfig` under "Example Connection Configuration". */
    ESP_ERROR_CHECK(example_connect());

    mqtt_app_start();

    xTaskCreate(sensor_publish_task, "sensor_publish", 4096, NULL, 5, NULL);
}
