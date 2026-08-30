idf_component_register(
    SRCS "app_main.c"
    PRIV_REQUIRES esp_wifi esp_event nvs_flash esp_netif esp_hw_support mqtt protocol_examples_common mpu6050
    INCLUDE_DIRS "."
)
