idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_event esp_http_client esp_netif esp_wifi json mbedtls nvs_flash
    PRIV_REQUIRES esp_timer
)
