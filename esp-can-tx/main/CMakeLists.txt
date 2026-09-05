idf_component_register(
    SRCS "esp_can_tx.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_twai nvs_flash esp_wifi esp_netif esp_event
)
