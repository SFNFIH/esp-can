idf_component_register(
    SRCS "esp_can_rx.c" "can_cli.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event console esp_timer
)
