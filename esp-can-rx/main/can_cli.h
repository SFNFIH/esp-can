#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "esp_err.h"
#include "can_espnow_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start UART REPL.
 * @param events  shared app event group (peer-learned bit set on auto-learn)
 * @param tx_queue CLI enqueues can_espnow_frame_t here for can_tx_task
 */
esp_err_t can_cli_start(EventGroupHandle_t events, QueueHandle_t tx_queue);

void can_cli_on_frame(const can_espnow_frame_t *frame, const uint8_t src_mac[6]);
void can_cli_note_drop(void);
void can_cli_note_tx_result(bool ok);
bool can_cli_get_peer(uint8_t mac_out[6]);

#ifdef __cplusplus
}
#endif
