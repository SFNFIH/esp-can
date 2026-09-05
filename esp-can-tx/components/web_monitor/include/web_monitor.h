#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "can_espnow_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint8_t  flags;
    uint8_t  data[CAN_ESPNOW_MAX_DATA];
    uint32_t seq;
    int64_t  ts_us;
} can_monitor_frame_t;

typedef struct {
    uint32_t rx_count;
    uint32_t fwd_ok;
    uint32_t fwd_fail;
    uint32_t drop_count;
} can_monitor_stats_t;

esp_err_t web_monitor_start(void);
void web_monitor_publish_frame(const can_monitor_frame_t *frame);
void web_monitor_set_stats(const can_monitor_stats_t *stats);

#ifdef __cplusplus
}
#endif
