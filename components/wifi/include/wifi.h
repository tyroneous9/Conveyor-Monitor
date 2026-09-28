#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"

/* Starts WiFi in station mode using the SSID/password from Kconfig.
 * Returns immediately; connection and reconnection (with exponential
 * backoff, never giving up) happen in the background via event handlers.
 * Requires NVS, esp_netif, and the default event loop to be initialized. */
void wifi_start(void);

/* Blocks until the station has an IPv4 address or `timeout` elapses.
 * Returns true if connected. */
bool wifi_wait_connected(TickType_t timeout);
