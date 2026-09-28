idf_component_register(
    SRCS "wifi.c" "app_main.c"
    PRIV_REQUIRES esp_wifi esp_event nvs_flash esp_netif esp_hw_support spi_flash mqtt esp_timer mpu6050 esp_driver_gpio
    INCLUDE_DIRS "."
)
