idf_component_register(
    SRCS "app_main.c"
    PRIV_REQUIRES esp_wifi esp_event nvs_flash
    INCLUDE_DIRS "."
)
