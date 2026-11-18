idf_component_register(
    SRCS "main.c" "font.c"
    INCLUDE_DIRS "."
    REQUIRES app_update esp_driver_gpio esp_driver_spi esp_event esp_http_client esp_http_server esp_netif esp_wifi mbedtls nvs_flash
    PRIV_REQUIRES esp_timer esp_psram
)
