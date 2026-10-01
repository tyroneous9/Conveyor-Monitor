idf_component_register(
    SRCS "app_main.c"
    PRIV_REQUIRES esp_event nvs_flash esp_netif esp_hw_support spi_flash mqtt wifi mpu6050 esp_driver_gpio
    INCLUDE_DIRS "."
)
