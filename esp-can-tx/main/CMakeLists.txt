idf_component_register(
    SRCS "esp_can_tx.c"
    REQUIRES esp_driver_twai esp_timer
    INCLUDE_DIRS "."
)
