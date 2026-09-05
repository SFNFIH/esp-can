#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "can_espnow_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t can_cli_start(void);
void can_cli_on_frame(const can_espnow_frame_t *frame, const uint8_t src_mac[6]);

#ifdef __cplusplus
}
#endif
