idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        can_espnow_proto
        freertos_app
        web_monitor
        esp_driver_twai
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_timer
)
