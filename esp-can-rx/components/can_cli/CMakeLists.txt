idf_component_register(
    SRCS "can_cli.c"
    INCLUDE_DIRS "include"
    REQUIRES
        can_espnow_proto
        freertos_app
        console
        esp_wifi
        esp_timer
        nvs_flash
)
