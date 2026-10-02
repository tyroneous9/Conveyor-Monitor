/*
 * WiFi station bring-up and reconnection.
 *
 * Replaces protocol_examples_common's example_connect(), which gives up after
 * a fixed number of consecutive failed reconnects and leaves the device
 * offline until a power cycle. Here reconnects retry forever, backing off
 * exponentially up to CONFIG_WIFI_MAX_BACKOFF_MS.
 */

#include "wifi.h"
#include "sdkconfig.h"
#include <string.h>
#include <sys/param.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_INITIAL_BACKOFF_MS 1000

static EventGroupHandle_t wifi_event_group;
static esp_timer_handle_t reconnect_timer;
static int backoff_ms = WIFI_INITIAL_BACKOFF_MS;

// Fired by reconnect_timer once the backoff delay has elapsed
static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Connecting to \"%s\"", CONFIG_WIFI_SSID);
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Disconnected, reconnecting");

        // Schedule the retry on a timer instead of sleeping, so the default event loop isn't blocked
        esp_timer_stop(reconnect_timer);
        esp_timer_start_once(reconnect_timer, (uint64_t)backoff_ms * 1000);
        backoff_ms = MIN(backoff_ms * 2, CONFIG_WIFI_MAX_BACKOFF_MS);
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // esp_netif already logs the assigned IP
        backoff_ms = WIFI_INITIAL_BACKOFF_MS;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_start(void)
{
    wifi_event_group = xEventGroupCreate();
    configASSERT(wifi_event_group != NULL);

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &reconnect_timer));

    esp_netif_create_default_wifi_sta();

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            // Reject networks weaker than WPA2 (an empty password means an open network is expected)
            .threshold.authmode = strlen(CONFIG_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, CONFIG_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));

    // Kconfig is the source of truth for credentials, so don't persist them to NVS
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Windows are published continuously (~2/s), so modem sleep only adds latency
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

bool wifi_wait_connected(TickType_t timeout)
{
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, timeout);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}
