idf_component_register(
    SRCS "web_monitor.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server freertos can_espnow_proto
)
